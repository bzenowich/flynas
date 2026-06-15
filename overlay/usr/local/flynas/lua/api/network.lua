local json = require("util.json")
local exec = require("util.exec")

local RCCONF = "/etc/rc.conf"
local ZONEDB = "/var/db/zoneinfo"

local _M = {}

-- Run a read-only shell command, return stdout or nil
local function capture(cmd)
    local pipe = io.popen(cmd .. " 2>/dev/null", "r")
    if not pipe then
        return nil
    end
    local output = pipe:read("*a")
    pipe:close()
    if output == "" then
        return nil
    end
    return output
end

local function read_file(path)
    local f = io.open(path, "r")
    if not f then
        return nil
    end
    local data = f:read("*a")
    f:close()
    return data
end

-- Last occurrence wins, matching how sh sources rc.conf
local function rcconf_get(key)
    local conf = read_file(RCCONF)
    if not conf then
        return nil
    end
    local value
    for line in conf:gmatch("[^\n]+") do
        local v = line:match("^%s*" .. key .. '%s*=%s*"([^"]*)"')
            or line:match("^%s*" .. key .. "%s*=%s*(%S+)")
        if v then
            value = v
        end
    end
    return value
end

local function valid_ipv4(s)
    if type(s) ~= "string" then
        return false
    end
    local a, b, c, d = s:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not a then
        return false
    end
    for _, o in ipairs({ a, b, c, d }) do
        if tonumber(o) > 255 then
            return false
        end
    end
    return true
end

local function valid_iface(s)
    return type(s) == "string" and #s <= 16 and s:match("^[a-z]+%d+$")
end

-- ifconfig prints the netmask as hex (0xffffff00)
local function hexmask_to_dotted(mask)
    local n = tonumber(mask)
    if not n then
        return mask
    end
    return string.format("%d.%d.%d.%d",
        math.floor(n / 2^24) % 256, math.floor(n / 2^16) % 256,
        math.floor(n / 2^8) % 256, n % 256)
end

-- The managed interface: first ifconfig_<name> entry in rc.conf,
-- falling back to the first non-loopback interface that is up.
local function primary_interface()
    local conf = read_file(RCCONF) or ""
    for line in conf:gmatch("[^\n]+") do
        local name = line:match('^%s*ifconfig_([a-z]+%d+)%s*=')
        if name then
            return name
        end
    end
    local out = capture("/sbin/ifconfig -a") or ""
    for line in out:gmatch("[^\n]+") do
        local name = line:match("^([a-z]+%d+):%s+flags=")
        if name and name ~= "lo0" then
            return name
        end
    end
    return nil
end

-- GET /api/network/config
function _M.get_config()
    local iface = primary_interface()
    if not iface then
        json.response({ error = "no network interface found" }, 500)
        return
    end

    local cfg = {
        interface = iface,
        dhcp = (rcconf_get("ifconfig_" .. iface) or ""):upper():find("DHCP") ~= nil,
    }

    local out = capture("/sbin/ifconfig " .. iface) or ""
    cfg.mac = out:match("ether%s+(%x+:%x+:%x+:%x+:%x+:%x+)")
    local ip, mask = out:match("inet%s+([%d%.]+)%s+netmask%s+(%S+)")
    cfg.ip = ip
    cfg.netmask = mask and hexmask_to_dotted(mask) or nil
    cfg.status = out:match("status:%s+(%S+)")

    local route = capture("/sbin/route -n get default") or ""
    cfg.gateway = route:match("gateway:%s+([%d%.]+)")

    json.response(cfg)
end

-- PUT /api/network/config  { dhcp } or { dhcp=false, ip, netmask, gateway }
-- Restarts netif: the response may not arrive if the address changes.
function _M.set_config(body)
    if not body or body.dhcp == nil then
        json.response({ error = "dhcp required" }, 400)
        return
    end

    local iface = body.interface or primary_interface()
    if not valid_iface(iface) then
        json.response({ error = "invalid interface" }, 400)
        return
    end

    local ok, err
    if body.dhcp then
        ok, err = exec.net_dhcp(iface)
    else
        if not (valid_ipv4(body.ip) and valid_ipv4(body.netmask)
                and valid_ipv4(body.gateway)) then
            json.response({ error = "ip, netmask and gateway must be valid IPv4 addresses" }, 400)
            return
        end
        ok, err = exec.net_static(iface, body.ip, body.netmask, body.gateway)
    end

    if not ok then
        ngx.log(ngx.ERR, "netconfig failed: ", err)
        json.response({ error = "network config failed: " .. (err or "?") }, 500)
        return
    end
    json.response({ status = "ok" })
end

-- GET /api/network/timezone
function _M.get_timezone()
    local zone = read_file(ZONEDB)
    -- No /var/db/zoneinfo means tzsetup never ran: UTC
    zone = zone and zone:gsub("%s+$", "") or "UTC"
    json.response({ timezone = zone })
end

-- PUT /api/network/timezone  { timezone = "America/New_York" }
function _M.set_timezone(body)
    local zone = body and body.timezone
    if type(zone) ~= "string" or #zone == 0 or #zone > 64
        or zone:find("%.%.") or not zone:match("^[%w_%+%-/]+$") then
        json.response({ error = "invalid timezone" }, 400)
        return
    end

    local ok, err = exec.set_timezone(zone)
    if not ok then
        ngx.log(ngx.ERR, "timezone failed: ", err)
        json.response({ error = "setting timezone failed: " .. (err or "?") }, 500)
        return
    end
    json.response({ status = "ok", timezone = zone })
end

-- GET /api/network/ntp
-- DragonFly base uses dntpd; servers live in rc.conf dntpd_flags
function _M.get_ntp()
    json.response({
        enabled = (rcconf_get("dntpd_enable") or "NO"):upper() == "YES",
        server = rcconf_get("dntpd_flags"),
    })
end

-- PUT /api/network/ntp  { server = "pool.ntp.org" }; empty disables
function _M.set_ntp(body)
    local server = body and body.server
    if server == "" then
        server = nil
    end
    if server ~= nil and (type(server) ~= "string" or #server > 255
        or not server:match("^[%w%.%-]+$")) then
        json.response({ error = "invalid NTP server" }, 400)
        return
    end

    local ok, err = exec.set_ntp(server)
    if not ok then
        ngx.log(ngx.ERR, "ntp failed: ", err)
        json.response({ error = "setting NTP failed: " .. (err or "?") }, 500)
        return
    end
    json.response({ status = "ok", enabled = server ~= nil, server = server })
end

-- ---- VM NAT network (flynas0 bridge) --------------------------

-- GET /api/network/vmnet
function _M.get_vmnet()
    local up = capture("/sbin/ifconfig flynas0") ~= nil
    json.response({
        enabled = up,
        subnet = "10.77.0.0/24",
        gateway = "10.77.0.1",
        uplink = primary_interface(),
    })
end

-- PUT /api/network/vmnet  { enabled }
function _M.set_vmnet(body)
    if not body or body.enabled == nil then
        json.response({ error = "enabled required" }, 400)
        return
    end
    if body.enabled then
        local uplink = primary_interface()
        if not uplink then
            json.response({ error = "no uplink interface found" }, 500)
            return
        end
        local ok, err = exec.vmbridge_up(uplink)
        if not ok then
            ngx.log(ngx.ERR, "vmbridge up failed: ", err)
            json.response({ error = "enable failed: " .. (err or "?") }, 500)
            return
        end
    else
        local ok, err = exec.vmbridge_down()
        if not ok then
            ngx.log(ngx.ERR, "vmbridge down failed: ", err)
            json.response({ error = "disable failed: " .. (err or "?") }, 500)
            return
        end
    end
    json.response({ status = "ok", enabled = body.enabled and true or false })
end

return _M
