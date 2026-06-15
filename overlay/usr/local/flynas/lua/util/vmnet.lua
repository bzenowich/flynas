-- VM NAT-network sync helpers: regenerate dnsmasq reservations and pf
-- port-forward rules from the DB, then poke the helper to reload. Safe
-- to call when the bridge is down (dnsmasq SIGHUP no-ops; pf anchor
-- load is skipped when there's no uplink).
local exec = require("util.exec")

local RUN_DIR = "/var/run/flynas"

local _M = {}

-- Default-route interface, or nil
function _M.uplink()
    local p = io.popen("/sbin/route -n get default 2>/dev/null")
    local out = p and p:read("*a") or ""
    if p then p:close() end
    return out:match("interface:%s*(%S+)")
end

-- Deterministic per-VM address on the bridge subnet.
function _M.vm_ip(id)
    return "10.77.0." .. (100 + id)
end

-- Rewrite dnsmasq's reservation file from current VMs, then SIGHUP.
function _M.sync_dhcp(conn)
    local vms = conn:query(
        "SELECT name, mac_address, ip_address FROM vms " ..
        "WHERE mac_address IS NOT NULL AND ip_address IS NOT NULL") or {}
    local f = io.open(RUN_DIR .. "/dhcp-hosts", "w")
    if not f then return end
    for _, v in ipairs(vms) do
        f:write(v.mac_address .. "," .. v.ip_address .. "," .. v.name .. "\n")
    end
    f:close()
    exec.dhcp_reload()
end

-- Rewrite the port-forward spec from current rows, then load the anchor.
function _M.sync_forwards(conn)
    local rows = conn:query(
        "SELECT f.proto, f.host_port, f.guest_port, v.ip_address " ..
        "FROM port_forwards f JOIN vms v ON v.id = f.vm_id " ..
        "WHERE v.ip_address IS NOT NULL") or {}
    local f = io.open(RUN_DIR .. "/fwd-spec", "w")
    if not f then return end
    for _, r in ipairs(rows) do
        f:write(r.proto .. "," .. r.host_port .. "," ..
            r.ip_address .. "," .. r.guest_port .. "\n")
    end
    f:close()
    local up = _M.uplink()
    if up then
        exec.pf_fwd(up)
    end
end

return _M
