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

-- Store connection for use by workers
ngx.shared_db_path = DB_PATH
