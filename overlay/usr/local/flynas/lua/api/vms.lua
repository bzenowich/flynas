local json = require("util.json")
local db = require("util.db")
local exec = require("util.exec")
local qmp = require("util.qmp")
local vmnet = require("util.vmnet")

local DB_PATH = "/usr/local/flynas/flynas.db"
local RUN_DIR = "/var/run/flynas"

local _M = {}

local function open_db()
    local conn = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
    end
    return conn
end

-- Each VM gets a dedicated tap; offset past low manually-created taps.
local function vm_tap(vm) return "tap" .. (1000 + vm.id) end

local function image_path(vm)
    if vm.volume and vm.volume ~= "" then
        return "/data/" .. vm.volume .. "/vms/" .. vm.name .. ".img"
    end
    return "/usr/local/flynas/vms/" .. vm.name .. ".img"
end

-- Live status straight from QMP (the socket only exists while the VM
-- runs). Don't gate on the pidfile — QEMU writes it 0600 root, which
-- www can't read.
local function live_status(vm)
    local st = qmp.status(vm.name)
    if not st then return "stopped" end
    if st == "paused" then return "suspended" end
    if st == "running" then return "running" end
    return st
end

local function gen_mac()
    return string.format("52:54:00:%02x:%02x:%02x",
        math.random(0, 255), math.random(0, 255), math.random(0, 255))
end

local function valid_name(s)
    return s and s:match("^[a-z_][a-z0-9_%-]*$") and #s <= 32
end

-- GET /api/vms
function _M.list()
    local conn = open_db()
    local vms = conn:query("SELECT * FROM vms ORDER BY name, id") or {}
    for _, vm in ipairs(vms) do
        vm.status = live_status(vm)
    end
    conn:close()
    json.response({ vms = vms })
end

-- GET /api/vms/:id
function _M.get(id)
    local conn = open_db()
    local vm = conn:query_one("SELECT * FROM vms WHERE id = ?", id)
    conn:close()
    if not vm then
        json.response({ error = "vm not found" }, 404)
        return
    end
    vm.status = live_status(vm)
    json.response(vm)
end

-- POST /api/vms  { name, cpus, ram_mb, disk_gb, volume?, iso_path? }
function _M.create(body)
    if not body or not valid_name(body.name) then
        json.response({ error = "valid name required (^[a-z_][a-z0-9_-]*$)" }, 400)
        return
    end
    local cpus = math.floor(tonumber(body.cpus) or 1)
    local ram = math.floor(tonumber(body.ram_mb) or 1024)
    local disk = math.floor(tonumber(body.disk_gb) or 20)
    if cpus < 1 or cpus > 256 then
        json.response({ error = "cpus out of range (1-256)" }, 400); return
    end
    if ram < 64 or ram > 1048576 then
        json.response({ error = "ram_mb out of range (64-1048576)" }, 400); return
    end
    if disk < 1 or disk > 4096 then
        json.response({ error = "disk_gb out of range (1-4096)" }, 400); return
    end
    local volume = body.volume
    if volume == "" then volume = nil end

    local conn = open_db()
    if conn:query_one("SELECT id FROM vms WHERE name = ?", body.name) then
        conn:close()
        json.response({ error = "a VM with that name already exists" }, 409)
        return
    end

    local ok, err = exec.vm_create(body.name, disk, volume or "-")
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "vmcreate failed: ", err)
        json.response({ error = "disk create failed: " .. (err or "?") }, 500)
        return
    end

    local mac = gen_mac()
    local rows, db_err = conn:query(
        "INSERT INTO vms (name, cpus, ram_mb, disk_gb, volume, iso_path, " ..
        "mac_address, status) VALUES (?, ?, ?, ?, ?, ?, ?, 'stopped') RETURNING *",
        body.name, cpus, ram, disk, volume, body.iso_path, mac)
    if not rows then
        conn:close()
        ngx.log(ngx.ERR, "vm insert failed: ", db_err)
        json.response({ error = "VM disk created but DB insert failed" }, 500)
        return
    end
    local vm = rows[1]
    -- Reserve a bridge IP so DHCP hands the guest a known address.
    local ip = vmnet.vm_ip(vm.id)
    conn:query("UPDATE vms SET ip_address = ? WHERE id = ?", ip, vm.id)
    vm.ip_address = ip
    vmnet.sync_dhcp(conn)
    conn:close()
    vm.status = "stopped"
    json.response(vm, 201)
end

-- DELETE /api/vms/:id
function _M.delete(id)
    local conn = open_db()
    local vm = conn:query_one("SELECT * FROM vms WHERE id = ?", id)
    if not vm then
        conn:close()
        json.response({ error = "vm not found" }, 404)
        return
    end
    if live_status(vm) ~= "stopped" then
        conn:close()
        json.response({ error = "stop the VM before deleting it" }, 409)
        return
    end
    local ok, err = exec.vm_delete(vm.name, vm.volume or "-")
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "vmdelete failed: ", err)
        json.response({ error = "delete failed: " .. (err or "?") }, 500)
        return
    end
    -- Delete the VM row first: vms.monitor_id is a FK to monitors(id),
    -- so the monitor can't be removed while the row still references it.
    conn:query("DELETE FROM vms WHERE id = ?", id)
    if vm.monitor_id then
        conn:query("DELETE FROM monitors WHERE id = ?", vm.monitor_id)
    end
    -- port_forwards rows cascade via FK; refresh DHCP + pf anchor.
    vmnet.sync_dhcp(conn)
    vmnet.sync_forwards(conn)
    conn:close()
    json.response({ status = "ok" })
end

-- POST /api/vms/:id/start
function _M.start(id)
    local conn = open_db()
    local vm = conn:query_one("SELECT * FROM vms WHERE id = ?", id)
    if not vm then
        conn:close()
        json.response({ error = "vm not found" }, 404)
        return
    end
    if live_status(vm) ~= "stopped" then
        conn:close()
        json.response({ error = "vm already running" }, 409)
        return
    end
    local ok, err = exec.vm_start(vm.name, vm.cpus, vm.ram_mb,
        image_path(vm), vm_tap(vm), vm.mac_address, vm.iso_path or "-")
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "vmstart failed: ", err)
        json.response({ error = "start failed: " .. (err or "?") }, 500)
        return
    end
    conn:query("UPDATE vms SET status = 'running' WHERE id = ?", id)
    conn:close()
    json.response({ status = "running" })
end

-- POST /api/vms/:id/stop  (graceful ACPI, then helper force-stops + cleans up)
function _M.stop(id)
    local conn = open_db()
    local vm = conn:query_one("SELECT * FROM vms WHERE id = ?", id)
    if not vm then
        conn:close()
        json.response({ error = "vm not found" }, 404)
        return
    end
    qmp.powerdown(vm.name)        -- best effort; guest may ignore
    local ok, err = exec.vm_stop(vm.name, vm_tap(vm))
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "vmstop failed: ", err)
        json.response({ error = "stop failed: " .. (err or "?") }, 500)
        return
    end
    conn:query("UPDATE vms SET status = 'stopped' WHERE id = ?", id)
    conn:close()
    json.response({ status = "stopped" })
end

-- POST /api/vms/:id/suspend
function _M.suspend(id)
    local conn = open_db()
    local vm = conn:query_one("SELECT * FROM vms WHERE id = ?", id)
    conn:close()
    if not vm then
        json.response({ error = "vm not found" }, 404)
        return
    end
    local ok, err = qmp.suspend(vm.name)
    if not ok then
        json.response({ error = "suspend failed: " .. (err or "?") }, 500)
        return
    end
    local c2 = open_db()
    c2:query("UPDATE vms SET status = 'suspended' WHERE id = ?", id)
    c2:close()
    json.response({ status = "suspended" })
end

-- POST /api/vms/:id/resume
function _M.resume(id)
    local conn = open_db()
    local vm = conn:query_one("SELECT * FROM vms WHERE id = ?", id)
    conn:close()
    if not vm then
        json.response({ error = "vm not found" }, 404)
        return
    end
    local ok, err = qmp.resume(vm.name)
    if not ok then
        json.response({ error = "resume failed: " .. (err or "?") }, 500)
        return
    end
    local c2 = open_db()
    c2:query("UPDATE vms SET status = 'running' WHERE id = ?", id)
    c2:close()
    json.response({ status = "running" })
end

-- ---- Port forwards --------------------------------------------

-- GET /api/vms/:id/forwards
function _M.list_forwards(vm_id)
    local conn = open_db()
    local rows = conn:query(
        "SELECT id, proto, host_port, guest_port FROM port_forwards " ..
        "WHERE vm_id = ? ORDER BY host_port", vm_id) or {}
    conn:close()
    json.response({ forwards = rows })
end

-- POST /api/vms/:id/forwards  { host_port, guest_port, proto? }
function _M.add_forward(vm_id, body)
    local hp = math.floor(tonumber(body and body.host_port) or 0)
    local gp = math.floor(tonumber(body and body.guest_port) or 0)
    local proto = (body and body.proto) or "tcp"
    if hp < 1 or hp > 65535 or gp < 1 or gp > 65535 then
        json.response({ error = "host_port and guest_port must be 1-65535" }, 400)
        return
    end
    if proto ~= "tcp" and proto ~= "udp" then
        json.response({ error = "proto must be tcp or udp" }, 400)
        return
    end
    local conn = open_db()
    local vm = conn:query_one("SELECT id FROM vms WHERE id = ?", vm_id)
    if not vm then
        conn:close()
        json.response({ error = "vm not found" }, 404)
        return
    end
    local rows = conn:query(
        "INSERT INTO port_forwards (vm_id, proto, host_port, guest_port) " ..
        "VALUES (?, ?, ?, ?) RETURNING id, proto, host_port, guest_port",
        vm_id, proto, hp, gp)
    vmnet.sync_forwards(conn)
    conn:close()
    json.response(rows and rows[1] or {}, 201)
end

-- DELETE /api/forwards/:id
function _M.delete_forward(fid)
    local conn = open_db()
    local row = conn:query_one("SELECT id FROM port_forwards WHERE id = ?", fid)
    if not row then
        conn:close()
        json.response({ error = "forward not found" }, 404)
        return
    end
    conn:query("DELETE FROM port_forwards WHERE id = ?", fid)
    vmnet.sync_forwards(conn)
    conn:close()
    json.response({ status = "ok" })
end

return _M
