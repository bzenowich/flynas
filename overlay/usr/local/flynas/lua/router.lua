local json = require("util.json")

local uri = ngx.var.uri
local method = ngx.req.get_method()

-- Strip /api/ prefix
local path = uri:match("^/api/(.*)$")
if not path then
    json.response({ error = "not found" }, 404)
end

-- Route dispatch table
local routes = {
    ["GET:health"] = function()
        json.response({ status = "ok" })
    end,
}

local key = method .. ":" .. path
local handler = routes[key]

if handler then
    handler()
else
    json.response({ error = "not found" }, 404)
end
