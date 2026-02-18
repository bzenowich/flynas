local json = require("util.json")
local auth = require("auth")
local auth_api = require("api.auth")

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
    ["POST:login"] = true,
}

-- Route dispatch table
local routes = {
    ["GET:health"] = function()
        json.response({ status = "ok" })
    end,

    ["POST:setup"] = function()
        auth_api.setup(read_body())
    end,

    ["POST:login"] = function()
        auth_api.login(read_body())
    end,

    ["GET:session"] = function()
        auth_api.session()
    end,

    ["POST:logout"] = function()
        auth_api.logout()
    end,
}

local key = method .. ":" .. path
local handler = routes[key]

if not handler then
    json.response({ error = "not found" }, 404)
end

-- Enforce authentication on protected routes
if not public_routes[key] then
    local user = auth.require_session()
    if not user then
        return
    end
    -- Store user for handler access
    ngx.ctx.user = user
end

handler()
