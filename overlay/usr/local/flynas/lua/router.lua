local json = require("util.json")
local auth = require("auth")
local auth_api = require("api.auth")
local users_api = require("api.users")
local groups_api = require("api.groups")
local settings_api = require("api.settings")
local dashboard_api = require("api.dashboard")

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
    ["POST:setup"] = true,
    ["POST:setup/confirm"] = true,
    ["POST:login"] = true,
    ["POST:login/verify"] = true,
}

-- Exact-match route dispatch table
local routes = {
    ["GET:health"] = function()
        json.response({ status = "ok" })
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
    { "POST",   "^users/(%d+)/totp/setup$",      function(id) users_api.totp_setup(tonumber(id)) end },
    { "POST",   "^users/(%d+)/totp/confirm$",    function(id) users_api.totp_confirm(tonumber(id), read_body()) end },
    { "DELETE", "^users/(%d+)/totp$",            function(id) users_api.totp_disable(tonumber(id)) end },
    { "PUT",    "^users/(%d+)/email$",           function(id) users_api.set_email(tonumber(id), read_body()) end },
    { "POST",   "^users/(%d+)/email/verify$",    function(id) users_api.verify_email(tonumber(id), read_body()) end },
    { "GET",    "^groups/(%d+)$",                function(id) groups_api.get(tonumber(id)) end },
    { "PUT",    "^groups/(%d+)$",                function(id) groups_api.update(tonumber(id), read_body()) end },
    { "DELETE", "^groups/(%d+)$",                function(id) groups_api.delete(tonumber(id)) end },
    { "PUT",    "^groups/(%d+)/members$",        function(id) groups_api.set_members(tonumber(id), read_body()) end },
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
