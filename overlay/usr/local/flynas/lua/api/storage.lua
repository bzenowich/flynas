local json = require("util.json")
local db = require("util.db")
local exec = require("util.exec")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

-- Run a read-only shell command, return stdout or nil
local function capture(cmd)
    local pipe = io.popen(cmd .. " 2>/dev/null", "r")
    if not pipe then
        return nil
    end
    local output = pipe:read("*a")
    pipe:close()
    if output == "" then
        return nil
    end
    return output
end

local function valid_volname(s)
    return s and #s >= 1 and #s <= 16 and s:match("^[a-z_][a-z0-9_%-]*$")
end

local function valid_disk(s)
    return s and #s <= 16 and s:match("^[a-z]+%d+$")
end

-- GET /api/disks
-- Like the dashboard disk list, but adds volume membership and
-- whether the disk backs a system filesystem.
function _M.disks()
    local list = capture("/sbin/sysctl -n kern.disks")
    if not list then
        json.response({ error = "kern.disks unavailable" }, 500)
        return
    end

    -- Map device -> volume name from the DB
    local membership = {}
    local conn = db.open(DB_PATH)
    if conn then
        local rows = conn:query(
            "SELECT vd.device, v.name FROM volume_disks vd " ..
            "JOIN volumes v ON v.id = vd.volume_id"
        )
        conn:close()
        for _, r in ipairs(rows or {}) do
            membership[r.device] = r.name
        end
    end

    local mounts = capture("/sbin/mount") or ""

    local disks = {}
    for name in list:gmatch("%S+") do
        if not (name:match("^cd%d") or name:match("^vn%d") or name:match("^md%d")) then
            local disk = { name = name, device = "/dev/" .. name }

            local geom = capture("/usr/local/flynas/bin/flynas-helper diskinfo " .. name)
            if geom then
                local cyl, hd, sec = geom:match("(%d+)%s+cyl%s+(%d+)%s+hd%s+(%d+)%s+sec")
                if cyl then
                    disk.size_bytes = tonumber(cyl) * tonumber(hd) * tonumber(sec) * 512
                end
            end

            local smart = capture("/usr/local/flynas/bin/flynas-helper smart " .. name)
            if smart then
                disk.serial = smart:match("Serial Number:%s+(%S+)")
                local health = smart:match("overall%-health self%-assessment test result:%s+(%S+)")
                disk.smart_health = health or "unknown"
            else
                disk.smart_health = "unavailable"
            end

            disk.volume = membership[disk.device]
            -- Mounted but not one of ours -> system disk (e.g. root).
            -- Match /dev/<name> not followed by a digit (vbd1 vs vbd10).
            if not disk.volume and
               mounts:match("/dev/" .. name .. "%f[%D]") then
                disk.system = true
            end

            disks[#disks + 1] = disk
        end
    end

    json.response({ disks = disks })
end

local function volume_with_usage(conn, vol)
    local disks = conn:query(
        "SELECT device FROM volume_disks WHERE volume_id = ?", vol.id
    )
    vol.disks = {}
    for _, d in ipairs(disks or {}) do
        vol.disks[#vol.disks + 1] = d.device
    end

    -- Live usage from df; absent when not mounted
    vol.mounted = false
    local out = capture("/bin/df -k " .. vol.mountpoint)
    if out then
        for line in out:gmatch("[^\n]+") do
            local total, used, avail, pct, mount =
                line:match("%s(%d+)%s+(%d+)%s+(%d+)%s+(%d+)%%%s+(%S+)")
            if total and mount == vol.mountpoint then
                vol.mounted = true
                vol.total_kb = tonumber(total)
                vol.used_kb = tonumber(used)
                vol.available_kb = tonumber(avail)
                vol.used_pct = tonumber(pct)
            end
        end
    end
    return vol
end

-- GET /api/volumes
function _M.list()
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local vols = conn:query(
        "SELECT id, name, mountpoint, root_device, scrub_enabled, created_at FROM volumes"
    ) or {}
    for _, v in ipairs(vols) do
        volume_with_usage(conn, v)
    end
    conn:close()

    json.response({ volumes = vols })
end

-- GET /api/volumes/:id
function _M.get(vol_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local vol = conn:query_one(
        "SELECT id, name, mountpoint, root_device, scrub_enabled, created_at " ..
        "FROM volumes WHERE id = ?", vol_id
    )
    if not vol then
        conn:close()
        json.response({ error = "volume not found" }, 404)
        return
    end

    volume_with_usage(conn, vol)
    conn:close()

    json.response(vol)
end

-- POST /api/volumes  { name, disks: ["vbd1", ...] }
-- DragonFly 6.4 has no hammer2 volume-add/del: the disk set is
-- fixed at creation, so all member disks are passed up front.
function _M.create(body)
    if not body or not body.name or type(body.disks) ~= "table" or #body.disks == 0 then
        json.response({ error = "name and disks[] required" }, 400)
        return
    end
    if not valid_volname(body.name) then
        json.response({ error = "invalid volume name" }, 400)
        return
    end
    for _, d in ipairs(body.disks) do
        if not valid_disk(d) then
            json.response({ error = "invalid disk name" }, 400)
            return
        end
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local existing = conn:query_one("SELECT id FROM volumes WHERE name = ?", body.name)
    if existing then
        conn:close()
        json.response({ error = "volume name already exists" }, 409)
        return
    end

    -- Helper re-checks that the disks are unused and formats them
    local disklist = table.concat(body.disks, ",")
    local ok, exec_err = exec.vol_create(body.name, disklist)
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "volcreate failed: ", exec_err)
        json.response({ error = "volume creation failed: " .. (exec_err or "?") }, 500)
        return
    end

    local mountpoint = "/data/" .. body.name
    local rows, db_err = conn:query(
        "INSERT INTO volumes (name, mountpoint, root_device) VALUES (?, ?, ?) " ..
        "RETURNING id, name, mountpoint, root_device, scrub_enabled, created_at",
        body.name, mountpoint, "/dev/" .. body.disks[1]
    )
    if not rows then
        conn:close()
        ngx.log(ngx.ERR, "volume insert failed: ", db_err)
        json.response({ error = "volume created but DB insert failed" }, 500)
        return
    end

    local vol = rows[1]
    for _, d in ipairs(body.disks) do
        conn:query(
            "INSERT INTO volume_disks (volume_id, device) VALUES (?, ?)",
            vol.id, "/dev/" .. d
        )
    end
    volume_with_usage(conn, vol)
    conn:close()

    json.response(vol, 201)
end

-- DELETE /api/volumes/:id
-- Unmounts and forgets the volume; disks are not wiped.
function _M.delete(vol_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local vol = conn:query_one("SELECT name FROM volumes WHERE id = ?", vol_id)
    if not vol then
        conn:close()
        json.response({ error = "volume not found" }, 404)
        return
    end

    local ok, exec_err = exec.vol_destroy(vol.name)
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "voldestroy failed: ", exec_err)
        json.response({ error = "volume removal failed: " .. (exec_err or "?") }, 500)
        return
    end

    conn:query("DELETE FROM volume_disks WHERE volume_id = ?", vol_id)
    conn:query("DELETE FROM snapshots WHERE volume_id = ?", vol_id)
    local _, db_err = conn:query("DELETE FROM volumes WHERE id = ?", vol_id)
    conn:close()

    if db_err then
        ngx.log(ngx.ERR, "volume delete failed: ", db_err)
        json.response({ error = "failed to delete volume" }, 500)
        return
    end

    json.response({ status = "ok" })
end

-- POST /api/volumes/:id/scrub
-- Runs hammer2 bulkfree synchronously; fast on small volumes but
-- will hold a worker on large ones (background job is a TODO).
function _M.scrub(vol_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local vol = conn:query_one("SELECT name FROM volumes WHERE id = ?", vol_id)
    conn:close()
    if not vol then
        json.response({ error = "volume not found" }, 404)
        return
    end

    local ok, exec_err = exec.vol_scrub(vol.name)
    if not ok then
        ngx.log(ngx.ERR, "scrub failed: ", exec_err)
        json.response({ error = "scrub failed: " .. (exec_err or "?") }, 500)
        return
    end

    json.response({ status = "ok" })
end

-- PUT /api/volumes/:id/scrub/schedule  { enabled }
function _M.scrub_schedule(vol_id, body)
    if not body or body.enabled == nil then
        json.response({ error = "enabled required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local vol = conn:query_one("SELECT id FROM volumes WHERE id = ?", vol_id)
    if not vol then
        conn:close()
        json.response({ error = "volume not found" }, 404)
        return
    end

    conn:query("UPDATE volumes SET scrub_enabled = ? WHERE id = ?",
        body.enabled and 1 or 0, vol_id)
    conn:close()

    json.response({ status = "ok" })
end

return _M
