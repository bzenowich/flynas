local json = require("util.json")
local db = require("util.db")
local exec = require("util.exec")
local vmnet = require("util.vmnet")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

local function open_db()
    local conn = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
    end
    return conn
end

local function gen_mac()
    return string.format("52:54:00:%02x:%02x:%02x",
        math.random(0, 255), math.random(0, 255), math.random(0, 255))
end

local function valid_name(s)
    return s and s:match("^[a-z_][a-z0-9_%-]*$") and #s <= 32
end

-- GET /api/apps  — the install catalog
function _M.list()
    local conn = open_db()
    local apps = conn:query(
        "SELECT id, name, description, default_cpus, default_ram_mb, " ..
        "default_disk_gb, monitor_type, monitor_port, monitor_path " ..
        "FROM app_templates ORDER BY name") or {}
    conn:close()
    json.response({ apps = apps })
end

-- POST /api/apps/:id/install
--   { name, cpus?, ram_mb?, disk_gb?, volume?, ip_address? }
-- Creates a VM from the template and, when an IP is supplied, an
-- auto-monitor pointed at the app (linked via vms.monitor_id so the
-- VM delete cascades it).
function _M.install(id, body)
    if not body or not valid_name(body.name) then
        json.response({ error = "valid VM name required (^[a-z_][a-z0-9_-]*$)" }, 400)
        return
    end
    local conn = open_db()
    local tpl = conn:query_one("SELECT * FROM app_templates WHERE id = ?", id)
    if not tpl then
        conn:close()
        json.response({ error = "app template not found" }, 404)
        return
    end
    if conn:query_one("SELECT id FROM vms WHERE name = ?", body.name) then
        conn:close()
        json.response({ error = "a VM with that name already exists" }, 409)
        return
    end

    local cpus = math.floor(tonumber(body.cpus) or tpl.default_cpus or 1)
    local ram = math.floor(tonumber(body.ram_mb) or tpl.default_ram_mb or 1024)
    local disk = math.floor(tonumber(body.disk_gb) or tpl.default_disk_gb or 20)
    local volume = body.volume
    if volume == "" then volume = nil end
    local ip = body.ip_address
    if ip == "" then ip = nil end

    local ok, err = exec.vm_create(body.name, disk, volume or "-")
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "app install vmcreate failed: ", err)
        json.response({ error = "disk create failed: " .. (err or "?") }, 500)
        return
    end

    local mac = gen_mac()
    local rows, db_err = conn:query(
        "INSERT INTO vms (name, cpus, ram_mb, disk_gb, volume, ip_address, " ..
        "mac_address, app_template, status) " ..
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'stopped') RETURNING *",
        body.name, cpus, ram, disk, volume, ip, mac, tpl.name)
    if not rows then
        conn:close()
        ngx.log(ngx.ERR, "app install vm insert failed: ", db_err)
        json.response({ error = "VM disk created but DB insert failed" }, 500)
        return
    end
    local vm = rows[1]

    -- Reserve a bridge IP (DHCP hands it to the guest) if none given.
    if not ip then
        ip = vmnet.vm_ip(vm.id)
        conn:query("UPDATE vms SET ip_address = ? WHERE id = ?", ip, vm.id)
        vm.ip_address = ip
    end

    -- Auto-monitor pointed at the app on the bridge.
    if ip then
        local target = string.format("http://%s:%d%s",
            ip, tpl.monitor_port or 80, tpl.monitor_path or "/")
        local mrows = conn:query(
            "INSERT INTO monitors (name, type, target, interval_sec, " ..
            "timeout_ms, expected_status, enabled) " ..
            "VALUES (?, ?, ?, 60, 5000, ?, 1) RETURNING id",
            vm.name .. "-" .. tpl.name:lower(),
            tpl.monitor_type or "http", target, tpl.monitor_expected or 200)
        if mrows and mrows[1] then
            conn:query("UPDATE vms SET monitor_id = ? WHERE id = ?",
                mrows[1].id, vm.id)
            vm.monitor_id = mrows[1].id
        end
    end

    vmnet.sync_dhcp(conn)
    conn:close()
    vm.status = "stopped"
    json.response(vm, 201)
end

return _M
