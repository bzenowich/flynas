local json = require("util.json")
local db = require("util.db")
local argon2 = require("util.argon2")
local auth = require("auth")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

function _M.setup(body)
    -- Check if setup is already done
    local done, err = auth.is_setup_done()
    if err then
        ngx.log(ngx.ERR, "setup check failed: ", err)
        json.response({ error = "internal error" }, 500)
        return
    end
    if done then
        json.response({ error = "setup already completed" }, 400)
        return
    end

    -- Validate input
    if not body or not body.username or not body.password then
        json.response({ error = "username and password required" }, 400)
        return
    end

    local username = body.username
    local password = body.password

    if #username < 1 or #password < 8 then
        json.response({ error = "password must be at least 8 characters" }, 400)
        return
    end

    -- Hash password
    local hash, hash_err = argon2.hash(password)
    if not hash then
        ngx.log(ngx.ERR, "argon2 hash failed: ", hash_err)
        json.response({ error = "internal error" }, 500)
        return
    end

    -- Create user
    local conn, db_err = db.open(DB_PATH)
    if not conn then
        ngx.log(ngx.ERR, "db open failed: ", db_err)
        json.response({ error = "internal error" }, 500)
        return
    end

    local rows, insert_err = conn:query(
        "INSERT INTO users (username, password_hash) VALUES (?, ?) RETURNING id, username, created_at",
        username, hash
    )
    conn:close()

    if not rows then
        ngx.log(ngx.ERR, "user insert failed: ", insert_err)
        json.response({ error = "failed to create user" }, 500)
        return
    end

    local user = rows[1]

    -- Create session
    local _, sess_err = auth.create_session(user.id)
    if sess_err then
        ngx.log(ngx.ERR, "session create failed: ", sess_err)
        json.response({ error = "internal error" }, 500)
        return
    end

    json.response({
        id = user.id,
        username = user.username,
        created_at = user.created_at,
    })
end

function _M.login(body)
    if not body or not body.username or not body.password then
        json.response({ error = "username and password required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        ngx.log(ngx.ERR, "db open failed: ", err)
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT id, username, password_hash, created_at FROM users WHERE username = ?",
        body.username
    )
    conn:close()

    if not user or not user.password_hash then
        json.response({ error = "invalid credentials" }, 401)
        return
    end

    if not argon2.verify(user.password_hash, body.password) then
        json.response({ error = "invalid credentials" }, 401)
        return
    end

    local _, sess_err = auth.create_session(user.id)
    if sess_err then
        ngx.log(ngx.ERR, "session create failed: ", sess_err)
        json.response({ error = "internal error" }, 500)
        return
    end

    json.response({
        id = user.id,
        username = user.username,
        created_at = user.created_at,
    })
end

function _M.session()
    local user = auth.require_session()
    if not user then
        return
    end

    json.response({
        id = user.id,
        username = user.username,
        created_at = user.created_at,
    })
end

function _M.logout()
    auth.destroy_session()
    json.response({ status = "ok" })
end

return _M
