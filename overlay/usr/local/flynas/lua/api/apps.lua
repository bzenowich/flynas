local json = require("util.json")
local db = require("util.db")
local exec = require("util.exec")
local vmnet = require("util.vmnet")
local oidc = require("api.oidc")
local cloudinit = require("util.cloudinit")

local DB_PATH = "/usr/local/flynas/flynas.db"
-- Cached base cloud image every app VM overlays (COW qcow2). Staged under
-- /usr/local/flynas/images/ (installer TODO to fetch it). §2.11 #1.
local BASE_IMAGE = "alpine.qcow2"

local _M = {}

-- Render a VM's guest user-data from its app template's cloud-init recipe
-- and the per-VM OIDC stash (client_id from oidc_clients, the once-only
-- secret + rendered redirect/issuer from app_provisioning). Returns
-- (user_data, nil) or (nil, err). Called at first Start (§2.11 step 6) to
-- feed exec.vm_seed. §2.11 step 4.
function _M.render_user_data(conn, vm, tpl)
    if not tpl.cloud_init then
        return nil, "app has no cloud-init recipe"
    end
    local prov = conn:query_one(
        "SELECT p.oidc_secret, p.redirect_uri, p.issuer, c.client_id " ..
        "FROM app_provisioning p " ..
        "LEFT JOIN oidc_clients c ON c.id = p.oidc_client_id " ..
        "WHERE p.vm_id = ?", vm.id) or {}
    -- The app's browser-facing base = the registered redirect_uri minus the
    -- app's OIDC callback path. Deriving ROOT_URL this way guarantees the
    -- callback Forgejo builds matches exactly what /authorize will accept.
    local root_url = prov.redirect_uri
    if prov.redirect_uri and tpl.oidc_redirect_path then
        root_url = prov.redirect_uri:sub(1,
            #prov.redirect_uri - #tpl.oidc_redirect_path) .. "/"
    end
    local vars = {
        APP_NAME      = vm.name,
        APP_PORT      = tostring(tpl.monitor_port or 80),
        ROOT_URL      = root_url,
        CLIENT_ID     = prov.client_id,
        CLIENT_SECRET = prov.oidc_secret,
        ISSUER        = prov.issuer,
    }
    return cloudinit.render(tpl.cloud_init, vars)
end

local function open_db()
    local conn = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
    end
    return conn
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

    -- App VMs boot a COW qcow2 overlay of the cached Alpine base; cloud-init
    -- installs the app on first Start (§2.11).
    local ok, err = exec.vm_create(body.name, disk, volume or "-", BASE_IMAGE)
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "app install vmcreate failed: ", err)
        json.response({ error = "disk create failed: " .. (err or "?") }, 500)
        return
    end

    local rows, db_err = conn:query(
        "INSERT INTO vms (name, cpus, ram_mb, disk_gb, volume, ip_address, " ..
        "app_template, status) " ..
        "VALUES (?, ?, ?, ?, ?, ?, ?, 'stopped') RETURNING *",
        body.name, cpus, ram, disk, volume, ip, tpl.name)
    if not rows then
        conn:close()
        ngx.log(ngx.ERR, "app install vm insert failed: ", db_err)
        json.response({ error = "VM disk created but DB insert failed" }, 500)
        return
    end
    local vm = rows[1]

    -- Reserve a bridge MAC, and an IP if none was given. Both derive from the
    -- row id, which only exists after the insert.
    local mac = vmnet.vm_mac(vm.id)
    conn:query("UPDATE vms SET mac_address = ? WHERE id = ?", mac, vm.id)
    vm.mac_address = mac
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

    -- Auto port-forward so the app is reachable from the LAN: host_port
    -- (deterministic, avoids collisions) → guest's published app port. The
    -- browser-facing base (external host + host_port) drives the OIDC
    -- redirect_uri / Forgejo ROOT_URL below. §2.11 #6.
    local app_base
    if ip then
        local host_port = 20000 + vm.id
        local guest_port = tpl.monitor_port or 80
        conn:query(
            "INSERT INTO port_forwards (vm_id, proto, host_port, guest_port) " ..
            "VALUES (?, 'tcp', ?, ?)", vm.id, host_port, guest_port)
        app_base = string.format("http://%s:%d",
            oidc.external_host(conn), host_port)
        vm.url = app_base .. "/"
    end

    -- Auto-mint an OIDC client for SSO-capable apps and stash it for guest
    -- provisioning (§2.10 #3). Best-effort: the VM already exists, so a mint
    -- failure is logged but never fails the install. The stashed redirect_uri
    -- uses the browser-facing app_base so it matches what the guest registers.
    if tpl.oidc_redirect_path then
        local sso, sso_err = oidc.provision_for_vm(conn, vm, tpl, app_base)
        if sso then
            vm.sso = sso
        elseif sso_err then
            ngx.log(ngx.ERR, "app install oidc provision failed: ", sso_err)
        end
    end

    vmnet.sync_dhcp(conn)
    vmnet.sync_forwards(conn)
    conn:close()
    vm.status = "stopped"
    json.response(vm, 201)
end

-- SUPERSEDED (§2.11 #6). Guest SSO delivery no longer needs an in-guest exec
-- channel: the OIDC config is baked into the NoCloud seed at first Start
-- (vms.start → render_user_data → exec.vm_seed), which sets delivered=1 and
-- NULLs the secret. Kept as a no-op for any lingering callers.
function _M.deliver_provisioning(vm_id)
    return nil, "delivery happens at first Start via the cloud-init seed (§2.11 #6)"
end

return _M
