local db = require("util.db")

local DB_PATH = "/usr/local/flynas/flynas.db"
local SCHEMA_PATH = "/usr/local/flynas/lua/schema.sql"

-- Open the database
local conn, err = db.open(DB_PATH)
if not conn then
    ngx.log(ngx.ERR, "failed to open database: ", err)
    return
end

-- Read and execute schema if tables don't exist
local row = conn:query_one("SELECT name FROM sqlite_master WHERE type='table' AND name='config'")
if not row then
    local f = io.open(SCHEMA_PATH, "r")
    if not f then
        ngx.log(ngx.ERR, "failed to open schema file: ", SCHEMA_PATH)
        return
    end
    local sql = f:read("*a")
    f:close()

    local ok, schema_err = conn:exec(sql)
    if not ok then
        ngx.log(ngx.ERR, "failed to apply schema: ", schema_err)
        return
    end
    ngx.log(ngx.NOTICE, "database schema initialized")
end

-- Migrations: add columns if missing
local function has_column(tbl, col)
    local rows = conn:query("PRAGMA table_info(" .. tbl .. ")")
    if not rows then return false end
    for _, r in ipairs(rows) do
        if r.name == col then return true end
    end
    return false
end

local function migrate_column(tbl, col, typedef)
    if not has_column(tbl, col) then
        local ok, merr = conn:exec(
            "ALTER TABLE " .. tbl .. " ADD COLUMN " .. col .. " " .. typedef
        )
        if ok then
            ngx.log(ngx.NOTICE, "migration: added " .. tbl .. "." .. col)
        else
            ngx.log(ngx.ERR, "migration failed for " .. tbl .. "." .. col .. ": ", merr)
        end
    end
end

-- Users table migrations
migrate_column("users", "email", "TEXT")
migrate_column("users", "email_verified", "INTEGER DEFAULT 0")
migrate_column("users", "totp_secret", "TEXT")
migrate_column("users", "totp_enabled", "INTEGER DEFAULT 0")
migrate_column("users", "is_admin", "INTEGER DEFAULT 0")

-- Sessions table migration
migrate_column("sessions", "state", "TEXT DEFAULT 'active'")

-- New tables (idempotent via IF NOT EXISTS)
conn:exec([[
    CREATE TABLE IF NOT EXISTS ssh_keys (
        id INTEGER PRIMARY KEY,
        user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
        label TEXT NOT NULL,
        public_key TEXT NOT NULL,
        created_at TEXT DEFAULT (datetime('now'))
    )
]])

conn:exec([[
    CREATE TABLE IF NOT EXISTS email_codes (
        id INTEGER PRIMARY KEY,
        user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
        code TEXT NOT NULL,
        purpose TEXT NOT NULL CHECK (purpose IN ('login', 'verify_email')),
        created_at TEXT DEFAULT (datetime('now')),
        expires_at TEXT NOT NULL,
        used INTEGER DEFAULT 0
    )
]])

-- Migrate existing pubkey data to ssh_keys table
local users_with_keys = conn:query(
    "SELECT id, username, pubkey FROM users WHERE pubkey IS NOT NULL AND pubkey != ''"
)
if users_with_keys then
    for _, u in ipairs(users_with_keys) do
        local existing = conn:query_one(
            "SELECT id FROM ssh_keys WHERE user_id = ? AND public_key = ?",
            u.id, u.pubkey
        )
        if not existing then
            conn:query(
                "INSERT INTO ssh_keys (user_id, label, public_key) VALUES (?, ?, ?)",
                u.id, "migrated", u.pubkey
            )
            ngx.log(ngx.NOTICE, "migrated pubkey for user: ", u.username)
        end
    end
end

conn:close()

-- Store connection for use by workers
ngx.shared_db_path = DB_PATH
