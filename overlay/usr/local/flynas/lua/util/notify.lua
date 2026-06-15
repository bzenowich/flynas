-- Fire monitor up/down notifications. Webhook = JSON POST over a
-- cosocket; email = reuse util.smtp with the global SMTP settings.
-- Called from the monitor worker timer (cosockets are allowed there).
local json = require("util.json")
local smtp = require("util.smtp")

local _M = {}

local function parse_url(url)
    local scheme, rest = url:match("^(https?)://(.+)$")
    if not scheme then return nil end
    local hostport, path = rest:match("^([^/]+)(/.*)$")
    if not hostport then hostport = rest; path = "/" end
    local host, port = hostport:match("^([^:]+):(%d+)$")
    if not host then
        host = hostport
        port = (scheme == "https") and 443 or 80
    end
    return scheme, host, tonumber(port), path
end

local function post_webhook(url, payload)
    local scheme, host, port, path = parse_url(url)
    if not scheme then return nil, "invalid webhook URL" end
    local sock = ngx.socket.tcp()
    sock:settimeout(10000)
    local ok, err = sock:connect(host, port)
    if not ok then return nil, "connect failed: " .. (err or "?") end
    if scheme == "https" then
        local sess, herr = sock:sslhandshake(nil, host, false)
        if not sess then sock:close(); return nil, "TLS failed: " .. (herr or "?") end
    end
    local body = json.encode(payload)
    local req = table.concat({
        "POST " .. path .. " HTTP/1.1",
        "Host: " .. host,
        "Content-Type: application/json",
        "Content-Length: " .. #body,
        "Connection: close",
        "User-Agent: FlyNAS-monitor",
        "", body,
    }, "\r\n")
    local _, serr = sock:send(req)
    if serr then sock:close(); return nil, "send failed: " .. serr end
    local status_line = sock:receive("*l")
    sock:close()
    local code = status_line and tonumber(status_line:match("^HTTP/%d%.%d%s+(%d+)"))
    if code and code >= 200 and code < 300 then return true end
    return nil, "webhook HTTP " .. tostring(code or "?")
end

-- channel: { type, config(JSON text) }
-- ctx: { monitor = {name,target}, status, message, smtp = <smtp config|nil> }
function _M.fire(channel, ctx)
    local ok, cfg = pcall(json.decode, channel.config)
    if not ok then return nil, "bad channel config" end

    local mon = ctx.monitor
    local subject = "[FlyNAS] " .. mon.name .. " is " .. string.upper(ctx.status)
    local text = string.format(
        "Monitor: %s\nTarget: %s\nStatus: %s\nDetail: %s\nTime: %s UTC",
        mon.name, mon.target, ctx.status, ctx.message or "",
        os.date("!%Y-%m-%d %H:%M:%S"))

    if channel.type == "webhook" then
        if not cfg.url then return nil, "webhook channel has no url" end
        return post_webhook(cfg.url, {
            monitor = mon.name, target = mon.target,
            status = ctx.status, message = ctx.message,
            text = text,
        })
    elseif channel.type == "email" then
        if not cfg.to then return nil, "email channel has no recipient" end
        if not ctx.smtp or not ctx.smtp.host then
            return nil, "SMTP not configured"
        end
        return smtp.send(ctx.smtp, cfg.to, subject, text)
    end
    return nil, "unknown channel type: " .. tostring(channel.type)
end

return _M
