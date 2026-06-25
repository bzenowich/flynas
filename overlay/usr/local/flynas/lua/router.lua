local json = require("util.json")
local auth = require("auth")
local auth_api = require("api.auth")
local users_api = require("api.users")
local groups_api = require("api.groups")
local settings_api = require("api.settings")
local dashboard_api = require("api.dashboard")
local storage_api = require("api.storage")
local network_api = require("api.network")
local backup_api = require("api.backup")
local monitors_api = require("api.monitors")
local vms_api = require("api.vms")
local apps_api = require("api.apps")
local oidc_api = require("api.oidc")

local uri = ngx.var.uri
local method = ngx.req.get_method()

-- Strip /api/ prefix
local path = uri:match("^/api/(.*)$")
if not path then
    json.response({ error = "not found" }, 404)
end

-- Read JSON request body for POST/PUT/PATCH
local function read_body()
    ngx.req.read_body()
    local raw = ngx.req.get_body_data()
    if not raw or raw == "" then
        return nil
    end
    local ok, data = pcall(json.decode, raw)
    if not ok then
        json.response({ error = "invalid JSON" }, 400)
        return nil
    end
    return data
end

-- Routes that do NOT require authentication
local public_routes = {
    ["GET:health"] = true,
    ["GET:setup/status"] = true,
    ["POST:setup"] = true,
    ["POST:setup/confirm"] = true,
    ["POST:login"] = true,
    ["POST:login/verify"] = true,
    -- OIDC provider endpoints (apps reach these directly; each does its
    -- own auth — client_secret/PKCE, Bearer token, or session-redirect).
    ["GET:oidc/.well-known/openid-configuration"] = true,
    ["GET:oidc/jwks"] = true,
    ["GET:oidc/authorize"] = true,
    ["POST:oidc/token"] = true,
    ["GET:oidc/userinfo"] = true,
}

-- Exact-match route dispatch table
local routes = {
    ["GET:health"] = function()
        json.response({ status = "ok" })
    end,

    ["GET:setup/status"] = function()
        auth_api.setup_status()
    end,

    ["POST:setup"] = function()
        auth_api.setup(read_body())
    end,

    ["POST:setup/confirm"] = function()
        auth_api.setup_confirm(read_body())
    end,

    ["POST:login"] = function()
        auth_api.login(read_body())
    end,

    ["POST:login/verify"] = function()
        auth_api.login_verify(read_body())
    end,

    ["GET:session"] = function()
        auth_api.session()
    end,

    ["POST:logout"] = function()
        auth_api.logout()
    end,

    -- Users
    ["GET:users"] = function()
        users_api.list()
    end,

    ["POST:users"] = function()
        users_api.create(read_body())
    end,

    -- Groups
    ["GET:groups"] = function()
        groups_api.list()
    end,

    ["POST:groups"] = function()
        groups_api.create(read_body())
    end,

    -- Dashboard
    ["GET:dashboard/system"] = function()
        dashboard_api.system()
    end,

    ["GET:dashboard/cpu"] = function()
        dashboard_api.cpu()
    end,

    ["GET:dashboard/memory"] = function()
        dashboard_api.memory()
    end,

    ["GET:dashboard/network"] = function()
        dashboard_api.network()
    end,

    ["GET:dashboard/disks"] = function()
        dashboard_api.disks()
    end,

    ["GET:dashboard/volumes"] = function()
        dashboard_api.volumes()
    end,

    -- Storage
    ["GET:disks"] = function()
        storage_api.disks()
    end,

    ["GET:volumes"] = function()
        storage_api.list()
    end,

    ["POST:volumes"] = function()
        storage_api.create(read_body())
    end,

    -- Backup
    ["GET:snapshots"] = function()
        backup_api.list_snapshots()
    end,

    ["POST:snapshots"] = function()
        backup_api.create_snapshot(read_body())
    end,

    ["GET:snapshots/schedule"] = function()
        backup_api.get_schedule()
    end,

    ["PUT:snapshots/schedule"] = function()
        backup_api.set_schedule(read_body())
    end,

    ["GET:s3"] = function()
        backup_api.list_s3()
    end,

    ["POST:s3"] = function()
        backup_api.create_s3(read_body())
    end,

    ["POST:backup/sync"] = function()
        backup_api.sync(read_body())
    end,

    ["GET:backup/status"] = function()
        backup_api.status()
    end,

    ["GET:restore/browse"] = function()
        backup_api.browse()
    end,

    -- Network
    ["GET:network/config"] = function()
        network_api.get_config()
    end,

    ["PUT:network/config"] = function()
        network_api.set_config(read_body())
    end,

    ["GET:network/timezone"] = function()
        network_api.get_timezone()
    end,

    ["PUT:network/timezone"] = function()
        network_api.set_timezone(read_body())
    end,

    ["GET:network/ntp"] = function()
        network_api.get_ntp()
    end,

    ["PUT:network/ntp"] = function()
        network_api.set_ntp(read_body())
    end,

    ["GET:network/vmnet"] = function()
        network_api.get_vmnet()
    end,

    ["PUT:network/vmnet"] = function()
        network_api.set_vmnet(read_body())
    end,

    -- Settings
    ["GET:settings/smtp"] = function()
        settings_api.get_smtp()
    end,

    ["PUT:settings/smtp"] = function()
        settings_api.set_smtp(read_body())
    end,

    ["POST:settings/smtp/test"] = function()
        settings_api.test_smtp(read_body())
    end,

    -- Monitoring
    ["GET:monitors"] = function()
        monitors_api.list()
    end,

    ["POST:monitors"] = function()
        monitors_api.create(read_body())
    end,

    ["GET:monitors/summary"] = function()
        monitors_api.summary()
    end,

    ["GET:notifications"] = function()
        monitors_api.list_channels()
    end,

    ["POST:notifications"] = function()
        monitors_api.create_channel(read_body())
    end,

    -- VMs
    ["GET:vms"] = function()
        vms_api.list()
    end,

    ["POST:vms"] = function()
        vms_api.create(read_body())
    end,

    -- Apps
    ["GET:apps"] = function()
        apps_api.list()
    end,

    -- OIDC provider (SSO over the SQLite directory)
    ["GET:oidc/.well-known/openid-configuration"] = function()
        oidc_api.discovery()
    end,

    ["GET:oidc/jwks"] = function()
        oidc_api.jwks()
    end,

    ["GET:oidc/authorize"] = function()
        oidc_api.authorize()
    end,

    ["POST:oidc/token"] = function()
        oidc_api.token()
    end,

    ["GET:oidc/userinfo"] = function()
        oidc_api.userinfo()
    end,

    -- OIDC client registration (admin session required)
    ["GET:oidc/clients"] = function()
        oidc_api.list_clients()
    end,

    ["POST:oidc/clients"] = function()
        oidc_api.create_client(read_body())
    end,
}

-- Pattern-based routes (checked if no exact match)
-- Each entry: { method, pattern, handler }
local pattern_routes = {
    { "GET",    "^users/(%d+)$",                 function(id) users_api.get(tonumber(id)) end },
    { "PUT",    "^users/(%d+)$",                 function(id) users_api.update(tonumber(id), read_body()) end },
    { "DELETE", "^users/(%d+)$",                 function(id) users_api.delete(tonumber(id)) end },
    { "GET",    "^users/(%d+)/ssh%-keys$",       function(id) users_api.list_ssh_keys(tonumber(id)) end },
    { "POST",   "^users/(%d+)/ssh%-keys$",       function(id) users_api.add_ssh_key(tonumber(id), read_body()) end },
    { "DELETE", "^users/(%d+)/ssh%-keys/(%d+)$", function(uid, kid) users_api.delete_ssh_key(tonumber(uid), tonumber(kid)) end },
    { "POST",   "^users/(%d+)/keypair$",         function(id) users_api.keypair(tonumber(id), read_body()) end },
    { "POST",   "^users/(%d+)/totp/setup$",      function(id) users_api.totp_setup(tonumber(id)) end },
    { "POST",   "^users/(%d+)/totp/confirm$",    function(id) users_api.totp_confirm(tonumber(id), read_body()) end },
    { "DELETE", "^users/(%d+)/totp$",            function(id) users_api.totp_disable(tonumber(id)) end },
    { "PUT",    "^users/(%d+)/email$",           function(id) users_api.set_email(tonumber(id), read_body()) end },
    { "POST",   "^users/(%d+)/email/verify$",    function(id) users_api.verify_email(tonumber(id), read_body()) end },
    { "GET",    "^volumes/(%d+)$",               function(id) storage_api.get(tonumber(id)) end },
    { "DELETE", "^volumes/(%d+)$",               function(id) storage_api.delete(tonumber(id)) end },
    { "POST",   "^volumes/(%d+)/scrub$",         function(id) storage_api.scrub(tonumber(id)) end },
    { "PUT",    "^volumes/(%d+)/scrub/schedule$", function(id) storage_api.scrub_schedule(tonumber(id), read_body()) end },
    { "DELETE", "^snapshots/(%d+)$",             function(id) backup_api.delete_snapshot(tonumber(id)) end },
    { "PUT",    "^s3/(%d+)$",                    function(id) backup_api.update_s3(tonumber(id), read_body()) end },
    { "DELETE", "^s3/(%d+)$",                    function(id) backup_api.delete_s3(tonumber(id)) end },
    { "GET",    "^groups/(%d+)$",                function(id) groups_api.get(tonumber(id)) end },
    { "PUT",    "^groups/(%d+)$",                function(id) groups_api.update(tonumber(id), read_body()) end },
    { "DELETE", "^groups/(%d+)$",                function(id) groups_api.delete(tonumber(id)) end },
    { "PUT",    "^groups/(%d+)/members$",        function(id) groups_api.set_members(tonumber(id), read_body()) end },
    { "GET",    "^monitors/(%d+)$",              function(id) monitors_api.get(tonumber(id)) end },
    { "PUT",    "^monitors/(%d+)$",              function(id) monitors_api.update(tonumber(id), read_body()) end },
    { "DELETE", "^monitors/(%d+)$",              function(id) monitors_api.delete(tonumber(id)) end },
    { "POST",   "^monitors/(%d+)/pause$",        function(id) monitors_api.pause(tonumber(id)) end },
    { "POST",   "^monitors/(%d+)/resume$",       function(id) monitors_api.resume(tonumber(id)) end },
    { "GET",    "^monitors/(%d+)/history$",      function(id) monitors_api.history(tonumber(id)) end },
    { "PUT",    "^monitors/(%d+)/notifications$", function(id) monitors_api.set_notifications(tonumber(id), read_body()) end },
    { "PUT",    "^notifications/(%d+)$",         function(id) monitors_api.update_channel(tonumber(id), read_body()) end },
    { "DELETE", "^notifications/(%d+)$",         function(id) monitors_api.delete_channel(tonumber(id)) end },
    { "GET",    "^vms/(%d+)$",                   function(id) vms_api.get(tonumber(id)) end },
    { "DELETE", "^vms/(%d+)$",                   function(id) vms_api.delete(tonumber(id)) end },
    { "POST",   "^vms/(%d+)/start$",             function(id) vms_api.start(tonumber(id)) end },
    { "POST",   "^vms/(%d+)/stop$",              function(id) vms_api.stop(tonumber(id)) end },
    { "POST",   "^vms/(%d+)/suspend$",           function(id) vms_api.suspend(tonumber(id)) end },
    { "POST",   "^vms/(%d+)/resume$",            function(id) vms_api.resume(tonumber(id)) end },
    { "GET",    "^vms/(%d+)/console$",           function(id) vms_api.console(tonumber(id)) end },
    { "GET",    "^vms/(%d+)/forwards$",          function(id) vms_api.list_forwards(tonumber(id)) end },
    { "POST",   "^vms/(%d+)/forwards$",          function(id) vms_api.add_forward(tonumber(id), read_body()) end },
    { "DELETE", "^forwards/(%d+)$",              function(id) vms_api.delete_forward(tonumber(id)) end },
    { "POST",   "^apps/(%d+)/install$",          function(id) apps_api.install(tonumber(id), read_body()) end },
    { "DELETE", "^oidc/clients/(%d+)$",          function(id) oidc_api.delete_client(tonumber(id)) end },
}

local key = method .. ":" .. path
local handler = routes[key]

if handler then
    -- Enforce authentication on protected routes
    if not public_routes[key] then
        local user = auth.require_session()
        if not user then
            return
        end
        ngx.ctx.user = user
    end
    handler()
else
    -- Try pattern routes (all require auth)
    for _, route in ipairs(pattern_routes) do
        if method == route[1] then
            local captures = { path:match(route[2]) }
            if #captures > 0 then
                local user = auth.require_session()
                if not user then
                    return
                end
                ngx.ctx.user = user
                route[3](unpack(captures))
                return
            end
        end
    end

    json.response({ error = "not found" }, 404)
end
