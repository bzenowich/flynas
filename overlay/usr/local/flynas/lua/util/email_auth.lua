local db = require("util.db")
local json = require("util.json")
local smtp = require("util.smtp")

local DB_PATH = "/usr/local/flynas/flynas.db"
local CODE_TTL = 600 -- 10 minutes
local RATE_LIMIT = 3 -- max codes per 15 minutes

local _M = {}

local function generate_code()
    local f = io.open("/dev/urandom", "rb")
    local bytes = f:read(4)
    f:close()
    local n = 0
    for i = 1, #bytes do
        n = n * 256 + bytes:byte(i)
    end
    return string.format("%06d", n % 1000000)
end

local function get_smtp_config(conn)
    local row = conn:query_one("SELECT value FROM config WHERE key = 'smtp'")
    if not row or not row.value then
        return nil
    end
    local ok, config = pcall(json.decode, row.value)
    if not ok then
        return nil
    end
    return config
end

function _M.send_code(user_id, email, purpose)
    local conn, err = db.open(DB_PATH)
    if not conn then
        return nil, err
    end

    -- Rate limit: check recent codes
    local recent = conn:query_one(
        "SELECT COUNT(*) as cnt FROM email_codes " ..
        "WHERE user_id = ? AND created_at > datetime('now', '-15 minutes')",
        user_id
    )
    if recent and recent.cnt >= RATE_LIMIT then
        conn:close()
        return nil, "too many codes requested, try again later"
    end

    local code = generate_code()

    local _, db_err = conn:query(
        "INSERT INTO email_codes (user_id, code, purpose, expires_at) " ..
        "VALUES (?, ?, ?, datetime('now', '+' || ? || ' seconds'))",
        user_id, code, purpose, CODE_TTL
    )
    if db_err then
        conn:close()
        return nil, db_err
    end

    -- Get SMTP config
    local smtp_config = get_smtp_config(conn)
    conn:close()

    if not smtp_config then
        return nil, "SMTP not configured"
    end

    local subject, body
    if purpose == "login" then
        subject = "FlyNAS Login Code"
        body = "Your login code is: " .. code .. "\n\nThis code expires in 10 minutes."
    else
        subject = "FlyNAS Email Verification"
        body = "Your verification code is: " .. code .. "\n\nThis code expires in 10 minutes."
    end

    local ok, send_err = smtp.send(smtp_config, email, subject, body)
    if not ok then
        return nil, "failed to send email: " .. send_err
    end

    return true
end

function _M.verify_code(user_id, code, purpose)
    local conn, err = db.open(DB_PATH)
    if not conn then
        return nil, err
    end

    local row = conn:query_one(
        "SELECT id FROM email_codes " ..
        "WHERE user_id = ? AND code = ? AND purpose = ? " ..
        "AND used = 0 AND expires_at > datetime('now') " ..
        "ORDER BY created_at DESC LIMIT 1",
        user_id, code, purpose
    )

    if not row then
        conn:close()
        return false
    end

    -- Mark as used
    conn:query("UPDATE email_codes SET used = 1 WHERE id = ?", row.id)
    conn:close()

    return true
end

return _M
