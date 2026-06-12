local db = require("util.db")
local json = require("util.json")

local DB_PATH = "/usr/local/flynas/flynas.db"
local SESSION_TTL = 86400  -- 24 hours in seconds
local PENDING_TTL = 600    -- 10 minutes for pending sessions

local _M = {}

function _M.generate_token()
    local f = io.open("/dev/urandom", "rb")
    local bytes = f:read(32)
    f:close()

    local hex = {}
    for i = 1, #bytes do
        hex[i] = string.format("%02x", string.byte(bytes, i))
    end
    return table.concat(hex)
end

function _M.generate_random_password()
    local f = io.open("/dev/urandom", "rb")
    local bytes = f:read(15)
    f:close()
    return ngx.encode_base64(bytes)
end

function _M.is_setup_done()
    local conn, err = db.open(DB_PATH)
    if not conn then
        return nil, err
    end
    local row, query_err = conn:query_one(
        "SELECT id FROM users WHERE is_admin = 1 AND totp_enabled = 1 LIMIT 1"
    )
    conn:close()
    if query_err then
        return nil, query_err
    end
    return row ~= nil
end

function _M.create_session(user_id, state)
    state = state or "active"
    local token = _M.generate_token()
    local ttl = (state == "active") and SESSION_TTL or PENDING_TTL

    local conn, err = db.open(DB_PATH)
    if not conn then
        return nil, err
    end

    local _, db_err = conn:query(
        "INSERT INTO sessions (token, user_id, state, expires_at) " ..
        "VALUES (?, ?, ?, datetime('now', '+' || ? || ' seconds'))",
        token, user_id, state, ttl
    )
    conn:close()

    if db_err then
        return nil, db_err
    end

    -- Only set cookie for active sessions
    if state == "active" then
        ngx.header["Set-Cookie"] = string.format(
            "flynas_session=%s; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=%d",
            token, SESSION_TTL
        )
    end

    return token
end

function _M.get_pending_session(token, expected_state)
    local conn, err = db.open(DB_PATH)
    if not conn then
        return nil, err
    end

    local row = conn:query_one(
        "SELECT user_id FROM sessions " ..
        "WHERE token = ? AND state = ? AND expires_at > datetime('now')",
        token, expected_state
    )

    if row then
        -- Delete the pending session (one-time use)
        conn:query("DELETE FROM sessions WHERE token = ?", token)
    end

    conn:close()
    return row and row.user_id or nil
end

function _M.destroy_session()
    local cookie = ngx.var.cookie_flynas_session
    if not cookie then
        return true
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        return nil, err
    end

    conn:query("DELETE FROM sessions WHERE token = ?", cookie)
    conn:close()

    -- Clear cookie
    ngx.header["Set-Cookie"] =
        "flynas_session=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0"

    return true
end

function _M.require_session()
    local cookie = ngx.var.cookie_flynas_session
    if not cookie then
        json.response({ error = "unauthorized" }, 401)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        ngx.log(ngx.ERR, "auth db error: ", err)
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT u.id, u.username, u.is_admin, u.created_at FROM users u " ..
        "JOIN sessions s ON s.user_id = u.id " ..
        "WHERE s.token = ? AND s.state = 'active' AND s.expires_at > datetime('now')",
        cookie
    )
    conn:close()

    if not user then
        json.response({ error = "unauthorized" }, 401)
        return
    end

    return user
end

return _M
