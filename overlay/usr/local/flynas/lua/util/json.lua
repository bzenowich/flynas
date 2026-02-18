local cjson = require("cjson")

local _M = {}

_M.encode = cjson.encode
_M.decode = cjson.decode

function _M.response(obj, status)
    ngx.status = status or 200
    ngx.header["Content-Type"] = "application/json"
    ngx.say(cjson.encode(obj))
    ngx.exit(ngx.status)
end

return _M
