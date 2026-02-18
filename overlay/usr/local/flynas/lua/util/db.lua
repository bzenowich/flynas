local ffi = require("ffi")

ffi.cdef[[
    typedef struct sqlite3 sqlite3;
    typedef struct sqlite3_stmt sqlite3_stmt;

    int sqlite3_open(const char *filename, sqlite3 **ppDb);
    int sqlite3_close(sqlite3 *db);
    int sqlite3_exec(sqlite3 *db, const char *sql,
        int (*callback)(void*,int,char**,char**),
        void *arg, char **errmsg);
    void sqlite3_free(void *ptr);
    const char *sqlite3_errmsg(sqlite3 *db);

    int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int nByte,
        sqlite3_stmt **ppStmt, const char **pzTail);
    int sqlite3_step(sqlite3_stmt *stmt);
    int sqlite3_finalize(sqlite3_stmt *stmt);
    int sqlite3_reset(sqlite3_stmt *stmt);

    int sqlite3_column_count(sqlite3_stmt *stmt);
    const char *sqlite3_column_name(sqlite3_stmt *stmt, int N);
    int sqlite3_column_type(sqlite3_stmt *stmt, int N);
    int sqlite3_column_int(sqlite3_stmt *stmt, int N);
    double sqlite3_column_double(sqlite3_stmt *stmt, int N);
    const char *sqlite3_column_text(sqlite3_stmt *stmt, int N);

    int sqlite3_bind_text(sqlite3_stmt *stmt, int idx,
        const char *value, int n, void(*)(void*));
    int sqlite3_bind_int(sqlite3_stmt *stmt, int idx, int value);
    int sqlite3_bind_double(sqlite3_stmt *stmt, int idx, double value);
    int sqlite3_bind_null(sqlite3_stmt *stmt, int idx);
]]

local SQLITE_OK = 0
local SQLITE_ROW = 100
local SQLITE_DONE = 101
local SQLITE_INTEGER = 1
local SQLITE_FLOAT = 2
local SQLITE_TEXT = 3
local SQLITE_NULL = 5
local SQLITE_TRANSIENT = ffi.cast("void(*)(void*)", -1)

local lib = ffi.load("sqlite3")

local _M = {}
local mt = { __index = _M }

function _M.open(path)
    local handle = ffi.new("sqlite3*[1]")
    local rc = lib.sqlite3_open(path, handle)
    if rc ~= SQLITE_OK then
        local msg = "failed to open database: " .. path
        if handle[0] ~= nil then
            msg = ffi.string(lib.sqlite3_errmsg(handle[0]))
            lib.sqlite3_close(handle[0])
        end
        return nil, msg
    end

    local self = setmetatable({ _db = handle[0] }, mt)

    -- Set WAL mode and enable foreign keys
    self:exec("PRAGMA journal_mode = WAL")
    self:exec("PRAGMA foreign_keys = ON")

    ffi.gc(handle, function(h)
        if h[0] ~= nil then
            lib.sqlite3_close(h[0])
            h[0] = nil
        end
    end)

    return self
end

function _M:close()
    if self._db ~= nil then
        lib.sqlite3_close(self._db)
        self._db = nil
    end
end

function _M:exec(sql)
    local errmsg = ffi.new("char*[1]")
    local rc = lib.sqlite3_exec(self._db, sql, nil, nil, errmsg)
    if rc ~= SQLITE_OK then
        local msg = ffi.string(errmsg[0])
        lib.sqlite3_free(errmsg[0])
        return nil, msg
    end
    return true
end

local function bind_params(stmt, params)
    for i, v in ipairs(params) do
        local t = type(v)
        if t == "string" then
            lib.sqlite3_bind_text(stmt, i, v, #v, SQLITE_TRANSIENT)
        elseif t == "number" then
            if v == math.floor(v) and v >= -2147483648 and v <= 2147483647 then
                lib.sqlite3_bind_int(stmt, i, v)
            else
                lib.sqlite3_bind_double(stmt, i, v)
            end
        elseif v == nil then
            lib.sqlite3_bind_null(stmt, i)
        end
    end
end

function _M:query(sql, ...)
    local stmt = ffi.new("sqlite3_stmt*[1]")
    local rc = lib.sqlite3_prepare_v2(self._db, sql, #sql, stmt, nil)
    if rc ~= SQLITE_OK then
        return nil, ffi.string(lib.sqlite3_errmsg(self._db))
    end

    local params = { ... }
    if #params > 0 then
        bind_params(stmt[0], params)
    end

    local rows = {}
    local ncol = lib.sqlite3_column_count(stmt[0])

    while true do
        rc = lib.sqlite3_step(stmt[0])
        if rc == SQLITE_DONE then
            break
        elseif rc ~= SQLITE_ROW then
            local msg = ffi.string(lib.sqlite3_errmsg(self._db))
            lib.sqlite3_finalize(stmt[0])
            return nil, msg
        end

        local row = {}
        for col = 0, ncol - 1 do
            local name = ffi.string(lib.sqlite3_column_name(stmt[0], col))
            local ctype = lib.sqlite3_column_type(stmt[0], col)
            if ctype == SQLITE_INTEGER then
                row[name] = lib.sqlite3_column_int(stmt[0], col)
            elseif ctype == SQLITE_FLOAT then
                row[name] = lib.sqlite3_column_double(stmt[0], col)
            elseif ctype == SQLITE_TEXT then
                row[name] = ffi.string(lib.sqlite3_column_text(stmt[0], col))
            else
                row[name] = nil
            end
        end
        rows[#rows + 1] = row
    end

    lib.sqlite3_finalize(stmt[0])
    return rows
end

function _M:query_one(sql, ...)
    local rows, err = self:query(sql, ...)
    if not rows then
        return nil, err
    end
    return rows[1]
end

return _M
