local json = require("util.json")
local db = require("util.db")
local auth = require("auth")
local totp = require("util.totp")
local exec = require("util.exec")
local email_auth = require("util.email_auth")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

local function valid_username(s)
    return s and #s >= 1 and #s <= 32 and s:match("^[a-z_][a-z0-9_%-]*$")
end

-- GET /api/setup/status — public: has initial setup been completed?
function _M.setup_status()
    local done, err = auth.is_setup_done()
    if err then
        ngx.log(ngx.ERR, "setup check failed: ", err)
        json.response({ error = "internal error" }, 500)
        return
    end
    json.response({ setup_done = done and true or false })
end

-- POST /api/setup — step 1: generate TOTP secret
function _M.setup(body)
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

    if not body or not body.username then
        json.response({ error = "username required" }, 400)
        return
    end

    if not valid_username(body.username) then
        json.response({ error = "invalid username" }, 400)
        return
    end

    local conn, db_err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    -- Check if user already exists in DB
    local existing = conn:query_one("SELECT id FROM users WHERE username = ?", body.username)
    local user_id

    if existing then
        user_id = existing.id
    else
        -- Create user record (no system account yet — that happens on confirm)
        local rows, insert_err = conn:query(
            "INSERT INTO users (username, is_admin) VALUES (?, 1) RETURNING id",
            body.username
        )
        if not rows then
            conn:close()
            ngx.log(ngx.ERR, "user insert failed: ", insert_err)
            json.response({ error = "failed to create user" }, 500)
            return
        end
        user_id = rows[1].id
    end

    -- Generate TOTP secret
    local secret = totp.generate_secret()
    conn:query("UPDATE users SET totp_secret = ? WHERE id = ?", secret, user_id)
    conn:close()

    -- Create pending setup session
    local setup_token, sess_err = auth.create_session(user_id, "setup_pending")
    if sess_err then
        json.response({ error = "internal error" }, 500)
        return
    end

    json.response({
        totp_secret = secret,
        totp_uri = totp.uri("FlyNAS", body.username, secret),
        setup_token = setup_token,
    })
end

-- POST /api/setup/confirm — step 2: verify TOTP, create system account
function _M.setup_confirm(body)
    local done, err = auth.is_setup_done()
    if err then
        json.response({ error = "internal error" }, 500)
        return
    end
    if done then
        json.response({ error = "setup already completed" }, 400)
        return
    end

    if not body or not body.setup_token or not body.totp_code then
        json.response({ error = "setup_token and totp_code required" }, 400)
        return
    end

    -- Validate pending session
    local user_id = auth.get_pending_session(body.setup_token, "setup_pending")
    if not user_id then
        json.response({ error = "invalid or expired setup token" }, 400)
        return
    end

    local conn, db_err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT id, username, totp_secret FROM users WHERE id = ?", user_id
    )
    if not user or not user.totp_secret then
        conn:close()
        json.response({ error = "user not found" }, 400)
        return
    end

    -- Verify TOTP code
    if not totp.verify(user.totp_secret, body.totp_code) then
        conn:close()
        json.response({ error = "invalid TOTP code" }, 400)
        return
    end

    -- Create system account with random password
    local sys_password = auth.generate_random_password()
    local ok, exec_err = exec.user_add(user.username, "/bin/sh")
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "useradd failed: ", exec_err)
        json.response({ error = "failed to create system account" }, 500)
        return
    end

    ok, exec_err = exec.set_password(user.username, sys_password)
    if not ok then
        ngx.log(ngx.ERR, "passwd failed: ", exec_err)
    end

    -- Hash password and update user
    local argon2 = require("util.argon2")
    local hash = argon2.hash(sys_password)

    conn:query(
        "UPDATE users SET password_hash = ?, totp_enabled = 1, is_admin = 1 WHERE id = ?",
        hash, user_id
    )
    conn:close()

    -- Create active session
    local token, sess_err = auth.create_session(user_id, "active")
    if sess_err then
        json.response({ error = "internal error" }, 500)
        return
    end

    json.response({
        id = user.id,
        username = user.username,
    })
end

-- POST /api/login — step 1: identify user, determine auth method
function _M.login(body)
    if not body or not body.username then
        json.response({ error = "username required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT id, username, email, email_verified, totp_enabled FROM users WHERE username = ?",
        body.username
    )
    conn:close()

    if not user then
        -- Don't reveal whether user exists — return generic response
        json.response({ error = "invalid credentials" }, 401)
        return
    end

    -- Determine auth method
    local auth_method
    if user.totp_enabled == 1 then
        auth_method = "totp"
    elseif user.email and user.email_verified == 1 then
        auth_method = "email"
    else
        json.response({ error = "no authentication method configured" }, 400)
        return
    end

    -- Create pending login session
    local login_token, sess_err = auth.create_session(user.id, "login_pending")
    if sess_err then
        json.response({ error = "internal error" }, 500)
        return
    end

    local response = {
        auth_method = auth_method,
        login_token = login_token,
    }

    -- Send email code if email auth
    if auth_method == "email" then
        local ok, send_err = email_auth.send_code(user.id, user.email, "login")
        if not ok then
            ngx.log(ngx.ERR, "failed to send login code: ", send_err)
            json.response({ error = "failed to send login code" }, 500)
            return
        end
        -- Mask email for hint
        local local_part, domain = user.email:match("^(.-)@(.+)$")
        if local_part and #local_part > 2 then
            response.email_hint = local_part:sub(1, 2) .. "***@" .. domain
        else
            response.email_hint = "***@" .. (domain or "")
        end
    end

    json.response(response)
end

-- POST /api/login/verify — step 2: verify code
function _M.login_verify(body)
    if not body or not body.login_token or not body.code then
        json.response({ error = "login_token and code required" }, 400)
        return
    end

    local user_id = auth.get_pending_session(body.login_token, "login_pending")
    if not user_id then
        json.response({ error = "invalid or expired login token" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT id, username, totp_enabled, totp_secret, email, email_verified FROM users WHERE id = ?",
        user_id
    )
    conn:close()

    if not user then
        json.response({ error = "user not found" }, 400)
        return
    end

    -- Verify code based on auth method
    local verified = false
    if user.totp_enabled == 1 and user.totp_secret then
        verified = totp.verify(user.totp_secret, body.code)
    elseif user.email and user.email_verified == 1 then
        verified = email_auth.verify_code(user_id, body.code, "login")
    end

    if not verified then
        json.response({ error = "invalid code" }, 400)
        return
    end

    -- Create active session
    local token, sess_err = auth.create_session(user_id, "active")
    if sess_err then
        json.response({ error = "internal error" }, 500)
        return
    end

    json.response({
        id = user.id,
        username = user.username,
    })
end

-- GET /api/session
function _M.session()
    local user = auth.require_session()
    if not user then
        return
    end

    json.response({
        id = user.id,
        username = user.username,
        is_admin = user.is_admin,
        created_at = user.created_at,
    })
end

-- POST /api/logout
function _M.logout()
    auth.destroy_session()
    json.response({ status = "ok" })
end

return _M
