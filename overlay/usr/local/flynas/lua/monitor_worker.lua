-- Background service monitor. Runs in worker 0 only via a 10s
-- ngx.timer.every. Each tick: find monitors whose interval has
-- elapsed, probe them concurrently (ngx.thread), record events,
-- fire notifications on status change, and hourly-prune old events.
local db = require("util.db")
local probe = require("util.probe")
local notify = require("util.notify")
local json = require("util.json")

local DB_PATH = "/usr/local/flynas/flynas.db"
local CHECK_INTERVAL = 10        -- seconds between ticks
local PRUNE_INTERVAL = 3600      -- seconds between event prunes
local RETENTION_DAYS = 90

-- One timer is enough; only the first worker runs it.
if ngx.worker.id() ~= 0 then
    return
end

local last_prune = 0

local function due_monitors(conn)
    -- Due if never checked, or last check is older than interval_sec.
    return conn:query([[
        SELECT id, name, type, target, interval_sec, timeout_ms,
               keyword, expected_status
        FROM monitors m
        WHERE enabled = 1
        AND COALESCE(
            strftime('%s','now') - strftime('%s',
                (SELECT MAX(checked_at) FROM monitor_events e
                 WHERE e.monitor_id = m.id)),
            999999999) >= interval_sec
    ]]) or {}
end

local function last_status(conn, mid)
    local row = conn:query_one(
        "SELECT status FROM monitor_events WHERE monitor_id = ? " ..
        "ORDER BY checked_at DESC, id DESC LIMIT 1", mid)
    return row and row.status or nil
end

local function fire_notifications(conn, mon, status, message)
    local chans = conn:query([[
        SELECT c.id, c.type, c.config
        FROM notification_channels c
        JOIN monitor_notifications mn ON mn.channel_id = c.id
        WHERE mn.monitor_id = ?]], mon.id)
    if not chans or #chans == 0 then return end

    local smtp_cfg
    local srow = conn:query_one("SELECT value FROM config WHERE key = 'smtp'")
    if srow and srow.value then
        local ok, parsed = pcall(json.decode, srow.value)
        if ok then smtp_cfg = parsed end
    end

    for _, ch in ipairs(chans) do
        local ok, err = notify.fire(ch, {
            monitor = mon, status = status, message = message, smtp = smtp_cfg,
        })
        if not ok then
            ngx.log(ngx.ERR, "notify channel ", ch.id, " failed: ", err or "?")
        end
    end
end

local function tick()
    local conn = db.open(DB_PATH)
    if not conn then
        ngx.log(ngx.ERR, "monitor: cannot open db")
        return
    end

    local due = due_monitors(conn)

    -- Probe all due monitors concurrently; probes never touch the DB.
    local pending = {}
    for _, m in ipairs(due) do
        pending[#pending + 1] = {
            mon = m,
            prev = last_status(conn, m.id),
            co = ngx.thread.spawn(function() return probe.check(m) end),
        }
    end

    for _, p in ipairs(pending) do
        local ok, status, rt, msg = ngx.thread.wait(p.co)
        if not ok then
            status, rt, msg = "down", 0, "probe error: " .. tostring(status)
        end
        conn:query(
            "INSERT INTO monitor_events (monitor_id, status, response_ms, message) " ..
            "VALUES (?, ?, ?, ?)", p.mon.id, status, rt, msg)
        -- Notify on the first result and on every state change after.
        if p.prev ~= status then
            fire_notifications(conn, p.mon, status, msg)
        end
    end

    local nowt = ngx.now()
    if nowt - last_prune > PRUNE_INTERVAL then
        conn:query(
            "DELETE FROM monitor_events WHERE checked_at < datetime('now', ?)",
            "-" .. RETENTION_DAYS .. " days")
        last_prune = nowt
    end

    conn:close()
end

local ok, err = ngx.timer.every(CHECK_INTERVAL, function(premature)
    if premature then return end
    local good, terr = pcall(tick)
    if not good then
        ngx.log(ngx.ERR, "monitor tick error: ", terr)
    end
end)

if not ok then
    ngx.log(ngx.ERR, "failed to start monitor timer: ", err)
else
    ngx.log(ngx.NOTICE, "monitor worker started (worker 0)")
end
