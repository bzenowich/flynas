local json = require("util.json")
local db = require("util.db")
local exec = require("util.exec")
local totp = require("util.totp")
local email_auth = require("util.email_auth")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

local function valid_username(s)
    return s and #s >= 1 and #s <= 32 and s:match("^[a-z_][a-z0-9_%-]*$")
end

-- GET /api/users
function _M.list()
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local rows = conn:query(
        "SELECT id, username, email, totp_enabled, is_admin, shell, ssh_enabled, created_at FROM users"
    )
    conn:close()

    json.response(rows or {})
end

-- POST /api/users
function _M.create(body)
    if not body or not body.username then
        json.response({ error = "username required" }, 400)
        return
    end

    if not valid_username(body.username) then
        json.response({ error = "invalid username" }, 400)
        return
    end

    local shell = body.shell or "/bin/sh"

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    -- Check for existing user
    local existing = conn:query_one("SELECT id FROM users WHERE username = ?", body.username)
    if existing then
        conn:close()
        json.response({ error = "username already exists" }, 409)
        return
    end

    -- Generate random password for system account
    local f = io.open("/dev/urandom", "rb")
    local bytes = f:read(15)
    f:close()
    local sys_password = ngx.encode_base64(bytes)

    -- Create system account
    local ok, exec_err = exec.user_add(body.username, shell)
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "useradd failed: ", exec_err)
        json.response({ error = "failed to create system account" }, 500)
        return
    end

    -- Set random password on system account
    ok, exec_err = exec.set_password(body.username, sys_password)
    if not ok then
        ngx.log(ngx.ERR, "passwd failed: ", exec_err)
    end

    -- Hash password for DB storage
    local argon2 = require("util.argon2")
    local hash = argon2.hash(sys_password)

    local rows, db_err = conn:query(
        "INSERT INTO users (username, password_hash, shell) VALUES (?, ?, ?) " ..
        "RETURNING id, username, shell, created_at",
        body.username, hash, shell
    )
    conn:close()

    if not rows then
        ngx.log(ngx.ERR, "user insert failed: ", db_err)
        json.response({ error = "failed to create user" }, 500)
        return
    end

    json.response(rows[1], 201)
end

-- GET /api/users/:id
function _M.get(user_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT id, username, email, email_verified, totp_enabled, is_admin, " ..
        "shell, ssh_enabled, created_at FROM users WHERE id = ?",
        user_id
    )
    if not user then
        conn:close()
        json.response({ error = "user not found" }, 404)
        return
    end

    -- Get groups
    local groups = conn:query(
        "SELECT g.id, g.name FROM groups g " ..
        "JOIN user_groups ug ON ug.group_id = g.id " ..
        "WHERE ug.user_id = ?",
        user_id
    )
    conn:close()

    user.groups = groups or {}
    json.response(user)
end

-- PUT /api/users/:id
function _M.update(user_id, body)
    if not body then
        json.response({ error = "request body required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one("SELECT username FROM users WHERE id = ?", user_id)
    if not user then
        conn:close()
        json.response({ error = "user not found" }, 404)
        return
    end

    if body.shell then
        local ok, exec_err = exec.user_mod(user.username, { shell = body.shell })
        if not ok then
            conn:close()
            json.response({ error = "failed to update shell: " .. exec_err }, 500)
            return
        end
        conn:query("UPDATE users SET shell = ? WHERE id = ?", body.shell, user_id)
    end

    if body.ssh_enabled ~= nil then
        conn:query("UPDATE users SET ssh_enabled = ? WHERE id = ?",
            body.ssh_enabled and 1 or 0, user_id)
    end

    conn:close()
    json.response({ status = "ok" })
end

-- DELETE /api/users/:id
function _M.delete(user_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT id, username, is_admin FROM users WHERE id = ?", user_id
    )
    if not user then
        conn:close()
        json.response({ error = "user not found" }, 404)
        return
    end

    -- Prevent deleting the last admin
    if user.is_admin == 1 then
        local admin_count = conn:query_one(
            "SELECT COUNT(*) as cnt FROM users WHERE is_admin = 1"
        )
        if admin_count and admin_count.cnt <= 1 then
            conn:close()
            json.response({ error = "cannot delete the last admin" }, 400)
            return
        end
    end

    -- Delete system account
    local ok, exec_err = exec.user_del(user.username)
    if not ok then
        ngx.log(ngx.ERR, "userdel failed: ", exec_err)
    end

    -- user_groups has no ON DELETE CASCADE; clear it first or the
    -- user delete fails the foreign key check
    conn:query("DELETE FROM user_groups WHERE user_id = ?", user_id)
    local _, db_err = conn:query("DELETE FROM users WHERE id = ?", user_id)
    conn:close()

    if db_err then
        ngx.log(ngx.ERR, "user delete failed: ", db_err)
        json.response({ error = "failed to delete user" }, 500)
        return
    end

    json.response({ status = "ok" })
end

-- GET /api/users/:id/ssh-keys
function _M.list_ssh_keys(user_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local keys = conn:query(
        "SELECT id, label, public_key, created_at FROM ssh_keys WHERE user_id = ?",
        user_id
    )
    conn:close()

    json.response(keys or {})
end

-- Sync SSH keys to authorized_keys file
local function sync_ssh_keys(conn, user_id, username)
    local keys = conn:query(
        "SELECT public_key FROM ssh_keys WHERE user_id = ?", user_id
    )
    if keys and #keys > 0 then
        local lines = {}
        for _, k in ipairs(keys) do
            lines[#lines + 1] = k.public_key
        end
        exec.write_ssh_keys(username, table.concat(lines, "\n") .. "\n")
    else
        exec.remove_ssh_keys(username)
    end
end

-- POST /api/users/:id/ssh-keys
function _M.add_ssh_key(user_id, body)
    if not body or not body.label or not body.public_key then
        json.response({ error = "label and public_key required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one("SELECT username FROM users WHERE id = ?", user_id)
    if not user then
        conn:close()
        json.response({ error = "user not found" }, 404)
        return
    end

    local rows, db_err = conn:query(
        "INSERT INTO ssh_keys (user_id, label, public_key) VALUES (?, ?, ?) " ..
        "RETURNING id, label, public_key, created_at",
        user_id, body.label, body.public_key
    )
    if not rows then
        conn:close()
        json.response({ error = "failed to add key" }, 500)
        return
    end

    sync_ssh_keys(conn, user_id, user.username)
    conn:close()

    json.response(rows[1], 201)
end

-- DELETE /api/users/:id/ssh-keys/:kid
function _M.delete_ssh_key(user_id, key_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one("SELECT username FROM users WHERE id = ?", user_id)
    if not user then
        conn:close()
        json.response({ error = "user not found" }, 404)
        return
    end

    local key = conn:query_one(
        "SELECT id FROM ssh_keys WHERE id = ? AND user_id = ?", key_id, user_id
    )
    if not key then
        conn:close()
        json.response({ error = "key not found" }, 404)
        return
    end

    conn:query("DELETE FROM ssh_keys WHERE id = ?", key_id)
    sync_ssh_keys(conn, user_id, user.username)
    conn:close()

    json.response({ status = "ok" })
end

-- Run a shell command, return output and success flag
local function capture(cmd)
    local pipe = io.popen(cmd .. " 2>&1", "r")
    if not pipe then
        return nil, false
    end
    local output = pipe:read("*a")
    local ok = pipe:close()
    return output, ok and true or false
end

-- Shell single-quote escaping for untrusted strings
local function shquote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- POST /api/users/:id/keypair
-- Generates an ed25519 keypair. The public key is stored and synced
-- to authorized_keys; the private key is returned AES-128 encrypted
-- with the caller's passphrase and never touches the database.
function _M.keypair(user_id, body)
    if not body or not body.passphrase or #body.passphrase < 8 then
        json.response({ error = "passphrase of at least 8 characters required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one("SELECT username FROM users WHERE id = ?", user_id)
    if not user then
        conn:close()
        json.response({ error = "user not found" }, 404)
        return
    end

    -- Random temp path so concurrent requests can't collide
    local f = io.open("/dev/urandom", "rb")
    local rnd = f:read(8)
    f:close()
    local hex = rnd:gsub(".", function(c) return string.format("%02x", c:byte()) end)
    local base = "/tmp/flynas-keygen-" .. hex

    local out, ok = capture(
        "/usr/bin/ssh-keygen -q -t ed25519 -N '' -C " ..
        shquote(user.username .. "@flynas") .. " -f " .. base .. " </dev/null"
    )
    if not ok then
        conn:close()
        os.remove(base)
        os.remove(base .. ".pub")
        ngx.log(ngx.ERR, "ssh-keygen failed: ", out or "?")
        json.response({ error = "key generation failed" }, 500)
        return
    end

    local pubf = io.open(base .. ".pub", "r")
    local public_key = pubf and pubf:read("*a") or nil
    if pubf then pubf:close() end

    -- Encrypt the private key; passphrase goes via stdin, never argv
    local enc, enc_ok = capture(
        "printf '%s' " .. shquote(body.passphrase) ..
        " | /usr/bin/openssl enc -aes-128-cbc -pbkdf2 -salt -base64" ..
        " -pass stdin -in " .. base
    )

    os.remove(base)
    os.remove(base .. ".pub")

    if not public_key or not enc_ok then
        conn:close()
        ngx.log(ngx.ERR, "key encryption failed: ", enc or "?")
        json.response({ error = "key encryption failed" }, 500)
        return
    end

    public_key = public_key:gsub("%s+$", "")

    local label = (body.label and #body.label > 0) and body.label
        or ("generated " .. os.date("!%Y-%m-%d"))
    local rows = conn:query(
        "INSERT INTO ssh_keys (user_id, label, public_key) VALUES (?, ?, ?) " ..
        "RETURNING id",
        user_id, label, public_key
    )
    sync_ssh_keys(conn, user_id, user.username)
    conn:close()

    json.response({
        key_id = rows and rows[1] and rows[1].id or nil,
        public_key = public_key,
        private_key_encrypted = enc,
        cipher = "aes-128-cbc",
        kdf = "pbkdf2",
        decrypt_hint = "openssl enc -d -aes-128-cbc -pbkdf2 -base64 -in <file>",
    }, 201)
end

-- POST /api/users/:id/totp/setup
function _M.totp_setup(user_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one("SELECT username FROM users WHERE id = ?", user_id)
    if not user then
        conn:close()
        json.response({ error = "user not found" }, 404)
        return
    end

    local secret = totp.generate_secret()
    conn:query("UPDATE users SET totp_secret = ? WHERE id = ?", secret, user_id)
    conn:close()

    local uri = totp.uri("FlyNAS", user.username, secret)

    json.response({
        totp_secret = secret,
        totp_uri = uri,
    })
end

-- POST /api/users/:id/totp/confirm
function _M.totp_confirm(user_id, body)
    if not body or not body.code then
        json.response({ error = "code required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local user = conn:query_one(
        "SELECT totp_secret FROM users WHERE id = ?", user_id
    )
    if not user or not user.totp_secret then
        conn:close()
        json.response({ error = "TOTP not set up" }, 400)
        return
    end

    if not totp.verify(user.totp_secret, body.code) then
        conn:close()
        json.response({ error = "invalid code" }, 400)
        return
    end

    conn:query("UPDATE users SET totp_enabled = 1 WHERE id = ?", user_id)
    conn:close()

    json.response({ status = "ok" })
end

-- DELETE /api/users/:id/totp
function _M.totp_disable(user_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    conn:query(
        "UPDATE users SET totp_enabled = 0, totp_secret = NULL WHERE id = ?",
        user_id
    )
    conn:close()

    json.response({ status = "ok" })
end

-- PUT /api/users/:id/email
function _M.set_email(user_id, body)
    if not body or not body.email then
        json.response({ error = "email required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    conn:query(
        "UPDATE users SET email = ?, email_verified = 0 WHERE id = ?",
        body.email, user_id
    )
    conn:close()

    -- Send verification code
    local ok, send_err = email_auth.send_code(user_id, body.email, "verify_email")
    if not ok then
        json.response({ status = "email set, verification pending", warning = send_err })
        return
    end

    json.response({ status = "verification code sent" })
end

-- POST /api/users/:id/email/verify
function _M.verify_email(user_id, body)
    if not body or not body.code then
        json.response({ error = "code required" }, 400)
        return
    end

    local verified, err = email_auth.verify_code(user_id, body.code, "verify_email")
    if not verified then
        json.response({ error = "invalid or expired code" }, 400)
        return
    end

    local conn, db_err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    conn:query("UPDATE users SET email_verified = 1 WHERE id = ?", user_id)
    conn:close()

    json.response({ status = "email verified" })
end

return _M
