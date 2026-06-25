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

-- VMs: which HAMMER2 volume holds the disk image (NULL = system dir)
migrate_column("vms", "volume", "TEXT")
-- VMs: which app template this VM was installed from (NULL = manual)
migrate_column("vms", "app_template", "TEXT")

-- App templates: description + default monitor definition
migrate_column("app_templates", "description", "TEXT")
migrate_column("app_templates", "monitor_type", "TEXT DEFAULT 'http'")
migrate_column("app_templates", "monitor_port", "INTEGER")
migrate_column("app_templates", "monitor_path", "TEXT DEFAULT '/'")
migrate_column("app_templates", "monitor_expected", "INTEGER DEFAULT 200")

-- Seed the app catalog (idempotent on the unique name). Each entry's
-- monitor_* fields become the auto-created monitor when installed.
local APP_CATALOG = {
    { "Seafile",     "Self-hosted file sync & share",        2, 2048, 40, 80,   "/api2/ping/" },
    { "CryptPad",    "Encrypted collaborative docs",         2, 2048, 20, 3000, "/" },
    { "Forgejo",     "Lightweight Git forge",                2, 2048, 30, 3000, "/" },
    { "VaultWarden", "Bitwarden-compatible password vault",  1, 1024, 10, 80,   "/alive" },
    { "Readeck",     "Read-it-later / bookmarks",            1, 1024, 10, 8000, "/" },
    { "Jellyfin",    "Media server",                         2, 4096, 30, 8096, "/web/" },
    { "RoundCube",   "Webmail (with smtp2go relay)",         1, 1024, 10, 80,   "/" },
    { "DokuWiki",    "Flat-file wiki",                       1, 1024, 10, 80,   "/" },
    { "Wekan",       "Kanban boards",                        2, 2048, 20, 80,   "/" },
}
for _, a in ipairs(APP_CATALOG) do
    conn:query(
        "INSERT OR IGNORE INTO app_templates " ..
        "(name, description, default_cpus, default_ram_mb, default_disk_gb, " ..
        "monitor_type, monitor_port, monitor_path, monitor_expected) " ..
        "VALUES (?, ?, ?, ?, ?, 'http', ?, ?, 200)",
        a[1], a[2], a[3], a[4], a[5], a[6], a[7])
end

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
    CREATE TABLE IF NOT EXISTS port_forwards (
        id INTEGER PRIMARY KEY,
        vm_id INTEGER REFERENCES vms(id) ON DELETE CASCADE,
        proto TEXT NOT NULL DEFAULT 'tcp',
        host_port INTEGER NOT NULL,
        guest_port INTEGER NOT NULL,
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

-- OIDC provider tables (idempotent). SQLite remains the user directory;
-- these project it to apps via OpenID Connect. See plan §2.10.
conn:exec([[
    CREATE TABLE IF NOT EXISTS oidc_clients (
        id INTEGER PRIMARY KEY,
        vm_id INTEGER REFERENCES vms(id) ON DELETE CASCADE,
        name TEXT NOT NULL,
        client_id TEXT UNIQUE NOT NULL,
        client_secret_hash TEXT NOT NULL,
        redirect_uris TEXT NOT NULL,
        created_at TEXT DEFAULT (datetime('now'))
    )
]])

conn:exec([[
    CREATE TABLE IF NOT EXISTS app_grants (
        user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
        client_id INTEGER REFERENCES oidc_clients(id) ON DELETE CASCADE,
        role TEXT DEFAULT 'user',
        PRIMARY KEY (user_id, client_id)
    )
]])

conn:exec([[
    CREATE TABLE IF NOT EXISTS oidc_codes (
        code TEXT PRIMARY KEY,
        client_id TEXT NOT NULL,
        user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
        redirect_uri TEXT NOT NULL,
        nonce TEXT,
        scope TEXT,
        code_challenge TEXT,
        code_challenge_method TEXT,
        expires_at INTEGER NOT NULL
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
