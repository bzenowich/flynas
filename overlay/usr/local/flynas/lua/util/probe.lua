-- Service probes for the monitoring worker. Each check returns
--   status ("up"|"down"), response_ms (int), message (string)
-- HTTP/TCP use OpenResty cosockets (non-blocking); ping/dns shell
-- out via resty.shell (ngx.pipe, also non-blocking). All run inside
-- the ngx.timer callback in monitor_worker.lua.
local shell = require("resty.shell")

local _M = {}

local function now_ms()
    return ngx.now() * 1000
end

-- "https://host:port/path" -> scheme, host, port, path
local function parse_url(url)
    local scheme, rest = url:match("^(https?)://(.+)$")
    if not scheme then return nil end
    local hostport, path = rest:match("^([^/]+)(/.*)$")
    if not hostport then
        hostport = rest
        path = "/"
    end
    local host, port = hostport:match("^([^:]+):(%d+)$")
    if not host then
        host = hostport
        port = (scheme == "https") and 443 or 80
    end
    return scheme, host, tonumber(port), path
end

local function check_tcp(host, port, timeout_ms)
    local sock = ngx.socket.tcp()
    sock:settimeout(timeout_ms)
    local t0 = now_ms()
    local ok, err = sock:connect(host, port)
    local dt = math.floor(now_ms() - t0)
    if not ok then
        return "down", dt, "connect failed: " .. (err or "?")
    end
    sock:close()
    return "up", dt, "connected"
end

-- HTTP and keyword checks share this. keyword ~= nil enables body read.
local function check_http(target, timeout_ms, expected_status, keyword)
    local scheme, host, port, path = parse_url(target)
    if not scheme then
        return "down", 0, "invalid URL (need http:// or https://)"
    end
    local sock = ngx.socket.tcp()
    sock:settimeout(timeout_ms)
    local t0 = now_ms()
    local ok, err = sock:connect(host, port)
    if not ok then
        return "down", math.floor(now_ms() - t0), "connect failed: " .. (err or "?")
    end
    if scheme == "https" then
        local sess, herr = sock:sslhandshake(nil, host, false)
        if not sess then
            sock:close()
            return "down", math.floor(now_ms() - t0), "TLS failed: " .. (herr or "?")
        end
    end
    local req = "GET " .. path .. " HTTP/1.1\r\nHost: " .. host ..
        "\r\nConnection: close\r\nUser-Agent: FlyNAS-monitor\r\nAccept: */*\r\n\r\n"
    local _, serr = sock:send(req)
    if serr then
        sock:close()
        return "down", math.floor(now_ms() - t0), "send failed: " .. serr
    end
    local status_line, lerr = sock:receive("*l")
    if not status_line then
        sock:close()
        return "down", math.floor(now_ms() - t0), "no response: " .. (lerr or "?")
    end
    local code = tonumber(status_line:match("^HTTP/%d%.%d%s+(%d+)"))
    local want_body = keyword and #keyword > 0
    local body = want_body and (sock:receive("*a") or "") or nil
    sock:close()
    local dt = math.floor(now_ms() - t0)
    if not code then
        return "down", dt, "bad status line"
    end
    if want_body then
        if body:find(keyword, 1, true) then
            return "up", dt, "keyword found (HTTP " .. code .. ")"
        end
        return "down", dt, "keyword missing (HTTP " .. code .. ")"
    end
    local want = expected_status or 200
    if code == want then
        return "up", dt, "HTTP " .. code
    end
    return "down", dt, "HTTP " .. code .. " (expected " .. want .. ")"
end

-- argv form (no shell) -> immune to target injection
local function run_argv(args, timeout_ms)
    local t0 = now_ms()
    local ok = shell.run(args, nil, timeout_ms + 1000, 8192)
    return ok, math.floor(now_ms() - t0)
end

local function check_ping(host, timeout_ms)
    local ok, dt = run_argv({ "ping", "-c", "1", "-W", tostring(timeout_ms), host }, timeout_ms)
    if ok then return "up", dt, "reachable" end
    return "down", dt, "unreachable"
end

local function check_dns(host, timeout_ms)
    local ok, dt = run_argv({ "drill", host }, timeout_ms)
    if ok then return "up", dt, "resolved" end
    return "down", dt, "resolution failed"
end

-- mon: { type, target, timeout_ms, expected_status, keyword }
function _M.check(mon)
    local timeout = tonumber(mon.timeout_ms) or 5000
    local t = mon.type
    if t == "tcp" then
        local host, port = mon.target:match("^([^:]+):(%d+)$")
        if not host then return "down", 0, "invalid host:port" end
        return check_tcp(host, tonumber(port), timeout)
    elseif t == "http" then
        return check_http(mon.target, timeout, mon.expected_status, nil)
    elseif t == "keyword" then
        return check_http(mon.target, timeout, mon.expected_status, mon.keyword or "")
    elseif t == "ping" then
        return check_ping(mon.target, timeout)
    elseif t == "dns" then
        return check_dns(mon.target, timeout)
    end
    return "down", 0, "unknown check type: " .. tostring(t)
end

return _M
