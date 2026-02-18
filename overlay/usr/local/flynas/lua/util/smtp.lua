local _M = {}

local function read_response(sock)
    local lines = {}
    while true do
        local line, err = sock:receive("*l")
        if not line then
            return nil, err
        end
        lines[#lines + 1] = line
        -- Last line of response: "NNN " (space, not dash)
        if line:match("^%d%d%d ") then
            break
        end
    end
    local full = table.concat(lines, "\n")
    local code = tonumber(full:sub(1, 3))
    return code, full
end

local function send_cmd(sock, cmd)
    local ok, err = sock:send(cmd .. "\r\n")
    if not ok then
        return nil, err
    end
    return read_response(sock)
end

-- config: { host, port, username, password, from, tls }
-- tls: true for STARTTLS (587) or direct TLS (465)
function _M.send(config, to, subject, body)
    local sock = ngx.socket.tcp()
    sock:settimeout(10000)

    local ok, err = sock:connect(config.host, config.port)
    if not ok then
        return nil, "connect failed: " .. err
    end

    -- Direct TLS (port 465)
    if config.port == 465 then
        local session, tls_err = sock:sslhandshake(nil, config.host, false)
        if not session then
            sock:close()
            return nil, "TLS handshake failed: " .. tls_err
        end
    end

    -- Read greeting
    local code, resp = read_response(sock)
    if not code or code ~= 220 then
        sock:close()
        return nil, "unexpected greeting: " .. (resp or "no response")
    end

    -- EHLO
    code, resp = send_cmd(sock, "EHLO flynas")
    if not code or code ~= 250 then
        sock:close()
        return nil, "EHLO failed: " .. (resp or "no response")
    end

    -- STARTTLS (port 587)
    if config.port == 587 and config.tls ~= false then
        code, resp = send_cmd(sock, "STARTTLS")
        if not code or code ~= 220 then
            sock:close()
            return nil, "STARTTLS failed: " .. (resp or "no response")
        end
        local session, tls_err = sock:sslhandshake(nil, config.host, false)
        if not session then
            sock:close()
            return nil, "TLS handshake failed: " .. tls_err
        end
        -- Re-EHLO after TLS
        code, resp = send_cmd(sock, "EHLO flynas")
        if not code or code ~= 250 then
            sock:close()
            return nil, "EHLO after TLS failed: " .. (resp or "no response")
        end
    end

    -- AUTH LOGIN
    if config.username and config.password then
        code, resp = send_cmd(sock, "AUTH LOGIN")
        if not code or code ~= 334 then
            sock:close()
            return nil, "AUTH LOGIN failed: " .. (resp or "no response")
        end
        code, resp = send_cmd(sock, ngx.encode_base64(config.username))
        if not code or code ~= 334 then
            sock:close()
            return nil, "AUTH username failed: " .. (resp or "no response")
        end
        code, resp = send_cmd(sock, ngx.encode_base64(config.password))
        if not code or code ~= 235 then
            sock:close()
            return nil, "AUTH password failed: " .. (resp or "no response")
        end
    end

    -- MAIL FROM
    code, resp = send_cmd(sock, "MAIL FROM:<" .. config.from .. ">")
    if not code or code ~= 250 then
        sock:close()
        return nil, "MAIL FROM failed: " .. (resp or "no response")
    end

    -- RCPT TO
    code, resp = send_cmd(sock, "RCPT TO:<" .. to .. ">")
    if not code or code ~= 250 then
        sock:close()
        return nil, "RCPT TO failed: " .. (resp or "no response")
    end

    -- DATA
    code, resp = send_cmd(sock, "DATA")
    if not code or code ~= 354 then
        sock:close()
        return nil, "DATA failed: " .. (resp or "no response")
    end

    -- Message content
    local message = string.format(
        "From: %s\r\nTo: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\n" ..
        "Content-Type: text/plain; charset=UTF-8\r\n\r\n%s\r\n.\r\n",
        config.from, to, subject, body
    )
    ok, err = sock:send(message)
    if not ok then
        sock:close()
        return nil, "send message failed: " .. err
    end

    code, resp = read_response(sock)
    if not code or code ~= 250 then
        sock:close()
        return nil, "message rejected: " .. (resp or "no response")
    end

    -- QUIT
    send_cmd(sock, "QUIT")
    sock:close()

    return true
end

return _M
