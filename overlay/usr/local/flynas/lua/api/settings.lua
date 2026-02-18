local json = require("util.json")
local db = require("util.db")
local smtp = require("util.smtp")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

-- GET /api/settings/smtp
function _M.get_smtp()
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local row = conn:query_one("SELECT value FROM config WHERE key = 'smtp'")
    conn:close()

    if not row or not row.value then
        json.response({})
        return
    end

    local ok, config = pcall(json.decode, row.value)
    if not ok then
        json.response({})
        return
    end

    -- Don't expose password
    config.password = nil
    json.response(config)
end

-- PUT /api/settings/smtp
function _M.set_smtp(body)
    if not body then
        json.response({ error = "request body required" }, 400)
        return
    end

    if not body.host or not body.port or not body.from then
        json.response({ error = "host, port, and from are required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    -- If password not provided, preserve existing one
    if not body.password then
        local existing = conn:query_one("SELECT value FROM config WHERE key = 'smtp'")
        if existing and existing.value then
            local ok, old_config = pcall(json.decode, existing.value)
            if ok and old_config.password then
                body.password = old_config.password
            end
        end
    end

    local config_json = json.encode(body)
    conn:query(
        "INSERT INTO config (key, value) VALUES ('smtp', ?) " ..
        "ON CONFLICT(key) DO UPDATE SET value = ?",
        config_json, config_json
    )
    conn:close()

    json.response({ status = "ok" })
end

-- POST /api/settings/smtp/test
function _M.test_smtp(body)
    if not body or not body.to then
        json.response({ error = "to address required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local row = conn:query_one("SELECT value FROM config WHERE key = 'smtp'")
    conn:close()

    if not row or not row.value then
        json.response({ error = "SMTP not configured" }, 400)
        return
    end

    local ok, config = pcall(json.decode, row.value)
    if not ok then
        json.response({ error = "invalid SMTP config" }, 500)
        return
    end

    local sent, send_err = smtp.send(
        config, body.to,
        "FlyNAS Test Email",
        "This is a test email from FlyNAS. If you received this, SMTP is working correctly."
    )

    if not sent then
        json.response({ error = "failed to send: " .. send_err }, 500)
        return
    end

    json.response({ status = "ok" })
end

return _M
