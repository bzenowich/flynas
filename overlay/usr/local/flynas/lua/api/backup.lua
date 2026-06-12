local json = require("util.json")
local db = require("util.db")
local exec = require("util.exec")

local DB_PATH = "/usr/local/flynas/flynas.db"
local RUN_DIR = "/usr/local/flynas/run"
local BACKUP_DIR = "/usr/local/flynas/backup"
local CRYFS_BATCH = "/usr/local/flynas/bin/cryfs-batch"

local _M = {}

local function open_db()
    local conn = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
    end
    return conn
end

-- ---- Snapshots -------------------------------------------------

-- GET /api/snapshots
function _M.list_snapshots()
    local conn = open_db()
    local snaps = conn:query(
        "SELECT s.id, s.volume_id, s.name, s.retention, s.created_at, " ..
        "v.name AS volume FROM snapshots s " ..
        "JOIN volumes v ON v.id = s.volume_id " ..
        "ORDER BY s.created_at DESC, s.id DESC"
    ) or {}
    conn:close()
    json.response({ snapshots = snaps })
end

-- POST /api/snapshots  { volume_id }
function _M.create_snapshot(body)
    if not body or not tonumber(body.volume_id) then
        json.response({ error = "volume_id required" }, 400)
        return
    end

    local conn = open_db()
    local vol = conn:query_one("SELECT id, name FROM volumes WHERE id = ?",
        tonumber(body.volume_id))
    if not vol then
        conn:close()
        json.response({ error = "volume not found" }, 404)
        return
    end

    -- snap-m-<stamp>: manual snapshot, exempt from retention pruning
    local name = os.date("snap-m-%Y%m%d%H%M%S")
    local ok, err = exec.snap_create(vol.name, name)
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "snapcreate failed: ", err)
        json.response({ error = "snapshot failed: " .. (err or "?") }, 500)
        return
    end

    local rows, db_err = conn:query(
        "INSERT INTO snapshots (volume_id, name) VALUES (?, ?) " ..
        "RETURNING id, volume_id, name, retention, created_at",
        vol.id, name
    )
    conn:close()
    if not rows then
        ngx.log(ngx.ERR, "snapshot insert failed: ", db_err)
        json.response({ error = "snapshot created but DB insert failed" }, 500)
        return
    end
    rows[1].volume = vol.name
    json.response(rows[1], 201)
end

-- DELETE /api/snapshots/:id
function _M.delete_snapshot(snap_id)
    local conn = open_db()
    local snap = conn:query_one(
        "SELECT s.id, s.name, v.name AS volume FROM snapshots s " ..
        "JOIN volumes v ON v.id = s.volume_id WHERE s.id = ?", snap_id)
    if not snap then
        conn:close()
        json.response({ error = "snapshot not found" }, 404)
        return
    end

    local ok, err = exec.snap_delete(snap.volume, snap.name)
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "snapdelete failed: ", err)
        json.response({ error = "snapshot delete failed: " .. (err or "?") }, 500)
        return
    end

    conn:query("DELETE FROM snapshots WHERE id = ?", snap_id)
    conn:close()
    json.response({ status = "ok" })
end

-- GET /api/snapshots/schedule
function _M.get_schedule()
    local conn = open_db()
    local row = conn:query_one(
        "SELECT value FROM config WHERE key = 'snapshot_schedule'")
    conn:close()

    local sched = { enabled = true }
    if row then
        local ok, parsed = pcall(json.decode, row.value)
        if ok and parsed.enabled ~= nil then
            sched.enabled = parsed.enabled and true or false
        end
    end
    json.response(sched)
end

-- PUT /api/snapshots/schedule  { enabled }
function _M.set_schedule(body)
    if not body or body.enabled == nil then
        json.response({ error = "enabled required" }, 400)
        return
    end

    local value = json.encode({ enabled = body.enabled and true or false })
    local conn = open_db()
    conn:query(
        "INSERT INTO config (key, value) VALUES ('snapshot_schedule', ?) " ..
        "ON CONFLICT(key) DO UPDATE SET value = ?",
        value, value
    )
    conn:close()
    json.response({ status = "ok" })
end

-- ---- S3 bucket configs ----------------------------------------
-- `name` is the bucket name on the remote; an endpoint starting
-- with "/" targets a local directory (offline testing).

local function bucket_public(b)
    return {
        id = b.id,
        name = b.name,
        endpoint = b.endpoint,
        access_key = b.access_key,
        has_password = (b.cryfs_password and #b.cryfs_password > 0) or false,
    }
end

-- GET /api/s3
function _M.list_s3()
    local conn = open_db()
    local rows = conn:query(
        "SELECT id, name, endpoint, access_key, cryfs_password FROM s3_buckets"
    ) or {}
    conn:close()
    local out = {}
    for _, b in ipairs(rows) do
        out[#out + 1] = bucket_public(b)
    end
    json.response({ buckets = out })
end

local function valid_bucket_body(body, require_secrets)
    if not body then return nil, "body required" end
    if not body.name or not body.name:match("^[a-z0-9][a-z0-9.%-]*$") then
        return nil, "invalid bucket name"
    end
    if not body.endpoint or #body.endpoint < 1 or body.endpoint:match("[%s'\"]") then
        return nil, "invalid endpoint"
    end
    if require_secrets then
        if not body.access_key or not body.secret_key then
            return nil, "access_key and secret_key required"
        end
        if not body.cryfs_password or #body.cryfs_password < 8 then
            return nil, "cryfs_password required (min 8 chars)"
        end
    end
    return true
end

-- POST /api/s3  { name, endpoint, access_key, secret_key, cryfs_password }
function _M.create_s3(body)
    -- Local-directory endpoints don't use S3 credentials
    local local_ep = body and body.endpoint and body.endpoint:sub(1, 1) == "/"
    if local_ep then
        body.access_key = body.access_key or "-"
        body.secret_key = body.secret_key or "-"
    end
    local ok, err = valid_bucket_body(body, true)
    if not ok then
        json.response({ error = err }, 400)
        return
    end

    local conn = open_db()
    local rows, db_err = conn:query(
        "INSERT INTO s3_buckets (name, endpoint, access_key, secret_key, cryfs_password) " ..
        "VALUES (?, ?, ?, ?, ?) RETURNING id, name, endpoint, access_key, cryfs_password",
        body.name, body.endpoint, body.access_key, body.secret_key,
        body.cryfs_password
    )
    conn:close()
    if not rows then
        ngx.log(ngx.ERR, "s3 insert failed: ", db_err)
        json.response({ error = "failed to save bucket" }, 500)
        return
    end
    json.response(bucket_public(rows[1]), 201)
end

-- PUT /api/s3/:id — omitted secret fields keep their stored values
function _M.update_s3(bucket_id, body)
    local ok, err = valid_bucket_body(body, false)
    if not ok then
        json.response({ error = err }, 400)
        return
    end

    local conn = open_db()
    local old = conn:query_one("SELECT * FROM s3_buckets WHERE id = ?", bucket_id)
    if not old then
        conn:close()
        json.response({ error = "bucket not found" }, 404)
        return
    end

    conn:query(
        "UPDATE s3_buckets SET name = ?, endpoint = ?, access_key = ?, " ..
        "secret_key = ?, cryfs_password = ? WHERE id = ?",
        body.name, body.endpoint,
        body.access_key or old.access_key,
        body.secret_key or old.secret_key,
        body.cryfs_password or old.cryfs_password,
        bucket_id
    )
    local row = conn:query_one(
        "SELECT id, name, endpoint, access_key, cryfs_password " ..
        "FROM s3_buckets WHERE id = ?", bucket_id)
    conn:close()
    json.response(bucket_public(row))
end

-- DELETE /api/s3/:id
function _M.delete_s3(bucket_id)
    local conn = open_db()
    local old = conn:query_one("SELECT id FROM s3_buckets WHERE id = ?", bucket_id)
    if not old then
        conn:close()
        json.response({ error = "bucket not found" }, 404)
        return
    end
    conn:query("DELETE FROM s3_buckets WHERE id = ?", bucket_id)
    conn:close()
    json.response({ status = "ok" })
end

-- ---- Backup sync ----------------------------------------------

local function read_status()
    local f = io.open(RUN_DIR .. "/backup-status.json", "r")
    if not f then
        return { state = "idle" }
    end
    local raw = f:read("*a")
    f:close()
    local ok, status = pcall(json.decode, raw)
    if not ok then
        return { state = "idle" }
    end
    return status
end

-- GET /api/backup/status
function _M.status()
    json.response(read_status())
end

-- POST /api/backup/sync  { bucket_id }
function _M.sync(body)
    if not body or not tonumber(body.bucket_id) then
        json.response({ error = "bucket_id required" }, 400)
        return
    end
    if read_status().state == "running" then
        json.response({ error = "backup already running" }, 409)
        return
    end

    local conn = open_db()
    local bucket = conn:query_one("SELECT * FROM s3_buckets WHERE id = ?",
        tonumber(body.bucket_id))
    if not bucket then
        conn:close()
        json.response({ error = "bucket not found" }, 404)
        return
    end
    if not bucket.cryfs_password or #bucket.cryfs_password == 0 then
        conn:close()
        json.response({ error = "bucket has no cryfs password" }, 400)
        return
    end

    local vols = conn:query("SELECT name, root_device FROM volumes") or {}
    conn:close()

    -- only mounted volumes can be snapshotted
    local mounted = {}
    local pipe = io.popen("/sbin/mount 2>/dev/null", "r")
    local mounts = pipe and pipe:read("*a") or ""
    if pipe then pipe:close() end
    for _, v in ipairs(vols) do
        if mounts:find("/data/" .. v.name, 1, true) then
            mounted[#mounted + 1] = { name = v.name, root_device = v.root_device }
        end
    end
    if #mounted == 0 then
        json.response({ error = "no mounted volumes to back up" }, 400)
        return
    end

    -- key=value lines: trivially parseable from sh, unlike JSON
    local lines = {
        "bucket_id=" .. bucket.id,
        "endpoint=" .. bucket.endpoint,
        "bucket=" .. bucket.name,
        "access_key=" .. (bucket.access_key or ""),
        "secret_key=" .. (bucket.secret_key or ""),
        "password=" .. bucket.cryfs_password,
    }
    for _, v in ipairs(mounted) do
        lines[#lines + 1] = "volume=" .. v.name
    end
    local job = table.concat(lines, "\n") .. "\n"
    local f = io.open(RUN_DIR .. "/backup-job.json", "w")
    if not f then
        json.response({ error = "cannot write job file" }, 500)
        return
    end
    f:write(job)
    f:close()

    local ok, err = exec.backup_sync()
    if not ok then
        os.remove(RUN_DIR .. "/backup-job.json")
        ngx.log(ngx.ERR, "backupsync failed: ", err)
        json.response({ error = "failed to start backup: " .. (err or "?") }, 500)
        return
    end
    json.response({ status = "started" }, 202)
end

-- ---- Restore browse -------------------------------------------

-- GET /api/restore/browse?bucket_id=N&volume=tank&path=/sub/dir
-- Lists immediate children of `path` from the local encrypted
-- block store (populated by the last sync to that bucket).
function _M.browse()
    local args = ngx.req.get_uri_args()
    local bucket_id = tonumber(args.bucket_id)
    local volume = args.volume
    local path = args.path or "/"
    if not bucket_id or not volume or not volume:match("^[a-z_][a-z0-9_%-]*$") then
        json.response({ error = "bucket_id and volume required" }, 400)
        return
    end
    if path:find("%.%.") or path:find("[%s'\"]") then
        json.response({ error = "invalid path" }, 400)
        return
    end

    local conn = open_db()
    local bucket = conn:query_one(
        "SELECT cryfs_password FROM s3_buckets WHERE id = ?", bucket_id)
    conn:close()
    if not bucket then
        json.response({ error = "bucket not found" }, 404)
        return
    end

    local blocks = BACKUP_DIR .. "/blocks/" .. bucket_id .. "/" .. volume
    local cfg = BACKUP_DIR .. "/cfg/" .. bucket_id .. "/" .. volume .. ".json"
    local cf = io.open(cfg, "r")
    if not cf then
        json.response({ error = "no backup found for this bucket/volume" }, 404)
        return
    end
    cf:close()

    local pwfile = RUN_DIR .. "/browse-pw." .. ngx.worker.pid()
    local pf = io.open(pwfile, "w")
    if not pf then
        json.response({ error = "cannot write password file" }, 500)
        return
    end
    pf:write(bucket.cryfs_password)
    pf:close()

    local rel = path:gsub("^/", ""):gsub("/$", "")
    local cmd = CRYFS_BATCH .. " list --remote '" .. blocks ..
        "' --config '" .. cfg .. "' --password-file '" .. pwfile ..
        "' --path '/" .. rel .. "' 2>&1"
    local lp = io.popen(cmd, "r")
    local out = lp and lp:read("*a") or ""
    if lp then lp:close() end
    os.remove(pwfile)

    if out:find("Error") or out:find("error:") then
        ngx.log(ngx.ERR, "cryfs-batch list failed: ", out)
        json.response({ error = "browse failed" }, 500)
        return
    end

    -- Lines: "<mode>  <date> <time>  <path>[/]  [(size)]".
    -- The listing is a recursive prefix dump; keep only immediate
    -- children of the requested path.
    local depth = 0
    if rel ~= "" then
        for _ in rel:gmatch("[^/]+") do depth = depth + 1 end
    end
    local entries = {}
    for line in out:gmatch("[^\n]+") do
        local mode, mtime, rest = line:match(
            "^([%-dlrwxsStT]+)%s%s(%d+%-%d+%-%d+ %d+:%d+)%s%s(.+)$")
        if mode then
            local epath, size = rest:match("^(.-)%s%s%((.+)%)$")
            local is_dir = false
            if not epath then
                epath = rest
                if epath:sub(-1) == "/" then
                    is_dir = true
                    epath = epath:sub(1, -2)
                end
            end
            local d = 0
            for _ in epath:gmatch("[^/]+") do d = d + 1 end
            local in_path = (rel == "") or
                (epath:sub(1, #rel + 1) == rel .. "/")
            if in_path and d == depth + 1 then
                entries[#entries + 1] = {
                    name = epath:match("[^/]+$"),
                    path = "/" .. epath,
                    dir = is_dir,
                    mtime = mtime,
                    size = size,
                }
            end
        end
    end
    json.response({ path = "/" .. rel, entries = entries })
end

return _M
