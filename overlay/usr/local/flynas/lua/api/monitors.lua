local json = require("util.json")
local db = require("util.db")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

local VALID_TYPES = { http = true, tcp = true, ping = true, dns = true, keyword = true }
local VALID_CHAN_TYPES = { email = true, webhook = true }

local function open_db()
    local conn = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
    end
    return conn
end

-- Percent of "up" events for monitor mid over a sqlite time window
-- like "24 hours". Returns nil when there are no events in the window.
local function uptime(conn, mid, window)
    local row = conn:query_one(
        "SELECT COUNT(*) AS total, " ..
        "SUM(CASE WHEN status = 'up' THEN 1 ELSE 0 END) AS up " ..
        "FROM monitor_events WHERE monitor_id = ? AND checked_at >= datetime('now', ?)",
        mid, "-" .. window)
    if not row or not row.total or row.total == 0 then
        return nil
    end
    return (row.up or 0) * 100.0 / row.total
end

local function current_status(m, last)
    if m.enabled == 0 then return "paused" end
    if last then return last.status end
    return "pending"
end

local function validate_monitor(body, require_all)
    if not body then return nil, "body required" end
    if require_all or body.name ~= nil then
        if not body.name or body.name == "" or #body.name > 64 then
            return nil, "name required (max 64 chars)"
        end
    end
    if require_all or body.type ~= nil then
        if not VALID_TYPES[body.type] then
            return nil, "type must be one of http, tcp, ping, dns, keyword"
        end
    end
    if require_all or body.target ~= nil then
        if not body.target or body.target == "" then
            return nil, "target required"
        end
        -- argv-based probes are injection-safe, but reject obviously
        -- malformed targets (whitespace / shell metacharacters).
        if body.target:match("[%s'\"`$;|&<>\\]") then
            return nil, "target contains invalid characters"
        end
    end
    return true
end

-- ---- Monitors --------------------------------------------------

-- GET /api/monitors
function _M.list()
    local conn = open_db()
    local mons = conn:query("SELECT * FROM monitors ORDER BY name, id") or {}
    for _, m in ipairs(mons) do
        local last = conn:query_one(
            "SELECT status, response_ms, checked_at FROM monitor_events " ..
            "WHERE monitor_id = ? ORDER BY checked_at DESC, id DESC LIMIT 1", m.id)
        m.status = current_status(m, last)
        m.last_response_ms = last and last.response_ms or nil
        m.last_checked = last and last.checked_at or nil
        m.uptime_24h = uptime(conn, m.id, "24 hours")
    end
    conn:close()
    json.response({ monitors = mons })
end

-- GET /api/monitors/summary
function _M.summary()
    local conn = open_db()
    local mons = conn:query("SELECT id, enabled FROM monitors") or {}
    local up, down, paused = 0, 0, 0
    for _, m in ipairs(mons) do
        if m.enabled == 0 then
            paused = paused + 1
        else
            local last = conn:query_one(
                "SELECT status FROM monitor_events WHERE monitor_id = ? " ..
                "ORDER BY checked_at DESC, id DESC LIMIT 1", m.id)
            if last and last.status == "down" then
                down = down + 1
            elseif last then
                up = up + 1
            end
        end
    end
    -- Overall 24h uptime across all events
    local row = conn:query_one(
        "SELECT COUNT(*) AS total, " ..
        "SUM(CASE WHEN status = 'up' THEN 1 ELSE 0 END) AS up " ..
        "FROM monitor_events WHERE checked_at >= datetime('now', '-24 hours')")
    conn:close()
    local overall = nil
    if row and row.total and row.total > 0 then
        overall = (row.up or 0) * 100.0 / row.total
    end
    json.response({
        total = #mons, up = up, down = down, paused = paused,
        uptime_24h = overall,
    })
end

-- GET /api/monitors/:id  — detail + recent events + assigned channels
function _M.get(mid)
    local conn = open_db()
    local m = conn:query_one("SELECT * FROM monitors WHERE id = ?", mid)
    if not m then
        conn:close()
        json.response({ error = "monitor not found" }, 404)
        return
    end
    local last = conn:query_one(
        "SELECT status, response_ms, checked_at FROM monitor_events " ..
        "WHERE monitor_id = ? ORDER BY checked_at DESC, id DESC LIMIT 1", mid)
    m.status = current_status(m, last)
    m.last_response_ms = last and last.response_ms or nil
    m.last_checked = last and last.checked_at or nil
    m.uptime_24h = uptime(conn, mid, "24 hours")
    m.uptime_7d = uptime(conn, mid, "7 days")
    m.uptime_30d = uptime(conn, mid, "30 days")
    m.events = conn:query(
        "SELECT status, response_ms, message, checked_at FROM monitor_events " ..
        "WHERE monitor_id = ? ORDER BY checked_at DESC, id DESC LIMIT 50", mid) or {}
    local chans = conn:query(
        "SELECT channel_id FROM monitor_notifications WHERE monitor_id = ?", mid) or {}
    local ids = {}
    for _, c in ipairs(chans) do ids[#ids + 1] = c.channel_id end
    m.channel_ids = ids
    conn:close()
    json.response(m)
end

-- POST /api/monitors
function _M.create(body)
    local ok, err = validate_monitor(body, true)
    if not ok then
        json.response({ error = err }, 400)
        return
    end
    local interval = math.max(10, tonumber(body.interval_sec) or 60)
    local timeout = math.max(100, tonumber(body.timeout_ms) or 5000)
    local conn = open_db()
    local rows, db_err = conn:query(
        "INSERT INTO monitors (name, type, target, interval_sec, timeout_ms, " ..
        "keyword, expected_status, enabled) VALUES (?, ?, ?, ?, ?, ?, ?, 1) " ..
        "RETURNING *",
        body.name, body.type, body.target, interval, timeout,
        body.keyword, tonumber(body.expected_status))
    conn:close()
    if not rows then
        ngx.log(ngx.ERR, "monitor insert failed: ", db_err)
        json.response({ error = "failed to create monitor" }, 500)
        return
    end
    json.response(rows[1], 201)
end

-- PUT /api/monitors/:id
function _M.update(mid, body)
    local ok, err = validate_monitor(body, false)
    if not ok then
        json.response({ error = err }, 400)
        return
    end
    local conn = open_db()
    local old = conn:query_one("SELECT * FROM monitors WHERE id = ?", mid)
    if not old then
        conn:close()
        json.response({ error = "monitor not found" }, 404)
        return
    end
    local interval = body.interval_sec ~= nil
        and math.max(10, tonumber(body.interval_sec) or old.interval_sec)
        or old.interval_sec
    local timeout = body.timeout_ms ~= nil
        and math.max(100, tonumber(body.timeout_ms) or old.timeout_ms)
        or old.timeout_ms
    conn:query(
        "UPDATE monitors SET name = ?, type = ?, target = ?, interval_sec = ?, " ..
        "timeout_ms = ?, keyword = ?, expected_status = ? WHERE id = ?",
        body.name or old.name, body.type or old.type, body.target or old.target,
        interval, timeout,
        body.keyword ~= nil and body.keyword or old.keyword,
        body.expected_status ~= nil and tonumber(body.expected_status) or old.expected_status,
        mid)
    local row = conn:query_one("SELECT * FROM monitors WHERE id = ?", mid)
    conn:close()
    json.response(row)
end

-- DELETE /api/monitors/:id  (events + channel links cascade)
function _M.delete(mid)
    local conn = open_db()
    local m = conn:query_one("SELECT id FROM monitors WHERE id = ?", mid)
    if not m then
        conn:close()
        json.response({ error = "monitor not found" }, 404)
        return
    end
    conn:query("DELETE FROM monitors WHERE id = ?", mid)
    conn:close()
    json.response({ status = "ok" })
end

local function set_enabled(mid, enabled)
    local conn = open_db()
    local m = conn:query_one("SELECT id FROM monitors WHERE id = ?", mid)
    if not m then
        conn:close()
        json.response({ error = "monitor not found" }, 404)
        return
    end
    conn:query("UPDATE monitors SET enabled = ? WHERE id = ?", enabled, mid)
    conn:close()
    json.response({ status = "ok" })
end

-- POST /api/monitors/:id/pause
function _M.pause(mid) set_enabled(mid, 0) end
-- POST /api/monitors/:id/resume
function _M.resume(mid) set_enabled(mid, 1) end

-- GET /api/monitors/:id/history?limit=N&before=<id>
function _M.history(mid)
    local args = ngx.req.get_uri_args()
    local limit = math.min(500, tonumber(args.limit) or 100)
    local before = tonumber(args.before)
    local conn = open_db()
    local events
    if before then
        events = conn:query(
            "SELECT id, status, response_ms, message, checked_at FROM monitor_events " ..
            "WHERE monitor_id = ? AND id < ? ORDER BY id DESC LIMIT ?",
            mid, before, limit)
    else
        events = conn:query(
            "SELECT id, status, response_ms, message, checked_at FROM monitor_events " ..
            "WHERE monitor_id = ? ORDER BY id DESC LIMIT ?", mid, limit)
    end
    conn:close()
    json.response({ events = events or {} })
end

-- ---- Notification channels ------------------------------------

-- GET /api/notifications
function _M.list_channels()
    local conn = open_db()
    local rows = conn:query("SELECT id, name, type, config FROM notification_channels ORDER BY name, id") or {}
    conn:close()
    json.response({ channels = rows })
end

local function validate_channel(body)
    if not body then return nil, "body required" end
    if not body.name or body.name == "" or #body.name > 64 then
        return nil, "name required (max 64 chars)"
    end
    if not VALID_CHAN_TYPES[body.type] then
        return nil, "type must be email or webhook"
    end
    if body.type == "email" then
        if not body.to or not body.to:match("^[^@%s]+@[^@%s]+$") then
            return nil, "valid recipient email required"
        end
    else
        if not body.url or not body.url:match("^https?://") then
            return nil, "webhook url must start with http:// or https://"
        end
    end
    return true
end

-- POST /api/notifications  { name, type, to|url }
function _M.create_channel(body)
    local ok, err = validate_channel(body)
    if not ok then
        json.response({ error = err }, 400)
        return
    end
    local cfg = body.type == "email" and { to = body.to } or { url = body.url }
    local conn = open_db()
    local rows, db_err = conn:query(
        "INSERT INTO notification_channels (name, type, config) VALUES (?, ?, ?) " ..
        "RETURNING id, name, type, config",
        body.name, body.type, json.encode(cfg))
    conn:close()
    if not rows then
        ngx.log(ngx.ERR, "channel insert failed: ", db_err)
        json.response({ error = "failed to create channel" }, 500)
        return
    end
    json.response(rows[1], 201)
end

-- PUT /api/notifications/:id
function _M.update_channel(cid, body)
    local ok, err = validate_channel(body)
    if not ok then
        json.response({ error = err }, 400)
        return
    end
    local cfg = body.type == "email" and { to = body.to } or { url = body.url }
    local conn = open_db()
    local old = conn:query_one("SELECT id FROM notification_channels WHERE id = ?", cid)
    if not old then
        conn:close()
        json.response({ error = "channel not found" }, 404)
        return
    end
    conn:query(
        "UPDATE notification_channels SET name = ?, type = ?, config = ? WHERE id = ?",
        body.name, body.type, json.encode(cfg), cid)
    local row = conn:query_one(
        "SELECT id, name, type, config FROM notification_channels WHERE id = ?", cid)
    conn:close()
    json.response(row)
end

-- DELETE /api/notifications/:id  (monitor links cascade)
function _M.delete_channel(cid)
    local conn = open_db()
    local old = conn:query_one("SELECT id FROM notification_channels WHERE id = ?", cid)
    if not old then
        conn:close()
        json.response({ error = "channel not found" }, 404)
        return
    end
    conn:query("DELETE FROM notification_channels WHERE id = ?", cid)
    conn:close()
    json.response({ status = "ok" })
end

-- PUT /api/monitors/:id/notifications  { channel_ids: [...] }
function _M.set_notifications(mid, body)
    if not body or type(body.channel_ids) ~= "table" then
        json.response({ error = "channel_ids array required" }, 400)
        return
    end
    local conn = open_db()
    local m = conn:query_one("SELECT id FROM monitors WHERE id = ?", mid)
    if not m then
        conn:close()
        json.response({ error = "monitor not found" }, 404)
        return
    end
    conn:query("DELETE FROM monitor_notifications WHERE monitor_id = ?", mid)
    for _, cid in ipairs(body.channel_ids) do
        local n = tonumber(cid)
        if n then
            conn:query(
                "INSERT OR IGNORE INTO monitor_notifications (monitor_id, channel_id) " ..
                "VALUES (?, ?)", mid, n)
        end
    end
    conn:close()
    json.response({ status = "ok" })
end

return _M
