-- OpenID Connect provider. Projects the SQLite user directory
-- (users/groups/user_groups) to installed apps so they share one set of
-- accounts. SQLite stays the source of truth; this is a read-mostly
-- front end. See plan §2.10.
--
-- Flow: app → GET authorize (reuses our flynas_session cookie + TOTP
-- login) → 302 back with a code → POST token (client_secret + PKCE) →
-- id_token/access_token → GET userinfo.
local json = require("util.json")
local db = require("util.db")
local argon2 = require("util.argon2")
local jwt = require("util.jwt")
local keys = require("util.oidc_keys")

local DB_PATH = "/usr/local/flynas/flynas.db"
local CODE_TTL = 60        -- seconds an auth code is valid
local TOKEN_TTL = 3600     -- id/access token lifetime

local _M = {}

local function open_db()
    local conn = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
    end
    return conn
end

local function rand_hex(n)
    local f = io.open("/dev/urandom", "rb")
    local b = f:read(n)
    f:close()
    local h = {}
    for i = 1, #b do h[i] = string.format("%02x", string.byte(b, i)) end
    return table.concat(h)
end

local function issuer()
    return "https://" .. (ngx.var.http_host or "localhost") .. "/api/oidc"
end

-- The active user for the current session cookie, or nil (no response).
local function session_user(conn)
    local cookie = ngx.var.cookie_flynas_session
    if not cookie then return nil end
    return conn:query_one(
        "SELECT u.id, u.username, u.email, u.email_verified, u.is_admin " ..
        "FROM users u JOIN sessions s ON s.user_id = u.id " ..
        "WHERE s.token = ? AND s.state = 'active' " ..
        "AND s.expires_at > datetime('now')", cookie)
end

-- Group names for a user, plus an "admin" pseudo-group when warranted.
local function user_groups(conn, user, app_role)
    local groups = {}
    local rows = conn:query(
        "SELECT g.name FROM groups g JOIN user_groups ug ON ug.group_id = g.id " ..
        "WHERE ug.user_id = ?", user.id)
    for _, r in ipairs(rows or {}) do groups[#groups + 1] = r.name end
    if user.is_admin == 1 or app_role == "admin" then
        groups[#groups + 1] = "admin"
    end
    return groups
end

-- Assemble OIDC claims for a user given the requested scope string.
local function build_claims(conn, user, client_id, scope)
    local now = ngx.time()
    local c = {
        iss = issuer(),
        sub = tostring(user.id),
        aud = client_id,
        iat = now,
        exp = now + TOKEN_TTL,
    }
    scope = scope or ""
    if scope:find("profile") then
        c.name = user.username
        c.preferred_username = user.username
    end
    if scope:find("email") and user.email then
        c.email = user.email
        c.email_verified = user.email_verified == 1
    end
    if scope:find("groups") then
        -- per-app role for the groups claim
        local grant = conn:query_one(
            "SELECT ag.role FROM app_grants ag " ..
            "JOIN oidc_clients oc ON oc.id = ag.client_id " ..
            "WHERE oc.client_id = ? AND ag.user_id = ?", client_id, user.id)
        c.groups = user_groups(conn, user, grant and grant.role)
    end
    return c
end

-- GET /api/oidc/.well-known/openid-configuration
function _M.discovery()
    local iss = issuer()
    json.response({
        issuer = iss,
        authorization_endpoint = iss .. "/authorize",
        token_endpoint = iss .. "/token",
        userinfo_endpoint = iss .. "/userinfo",
        jwks_uri = iss .. "/jwks",
        response_types_supported = { "code" },
        grant_types_supported = { "authorization_code" },
        subject_types_supported = { "public" },
        id_token_signing_alg_values_supported = { "RS256" },
        scopes_supported = { "openid", "profile", "email", "groups" },
        token_endpoint_auth_methods_supported = { "client_secret_post" },
        code_challenge_methods_supported = { "S256", "plain" },
    })
end

-- GET /api/oidc/jwks
function _M.jwks()
    local set, err = keys.jwks()
    if not set then
        json.response({ error = err or "no key" }, 500)
        return
    end
    json.response(set)
end

-- GET /api/oidc/authorize?client_id&redirect_uri&response_type=code
--     &scope&state&nonce&code_challenge&code_challenge_method
function _M.authorize()
    local a = ngx.req.get_uri_args()
    if a.response_type ~= "code" then
        json.response({ error = "unsupported_response_type" }, 400)
        return
    end
    if not a.client_id or not a.redirect_uri then
        json.response({ error = "invalid_request" }, 400)
        return
    end

    local conn = open_db()
    local client = conn:query_one(
        "SELECT id, redirect_uris FROM oidc_clients WHERE client_id = ?",
        a.client_id)
    if not client then
        conn:close()
        json.response({ error = "invalid_client" }, 401)
        return
    end

    -- Exact-match the redirect against the registered allowlist.
    local allowed = false
    for uri in (client.redirect_uris .. "\n"):gmatch("([^\n]*)\n") do
        if uri ~= "" and uri == a.redirect_uri then allowed = true break end
    end
    if not allowed then
        conn:close()
        json.response({ error = "invalid redirect_uri" }, 400)
        return
    end

    -- Reuse our own session. No session → bounce to the SPA login, which
    -- replays the original authorize URL via the `return` param.
    local user = session_user(conn)
    if not user then
        conn:close()
        local target = ngx.var.request_uri
        return ngx.redirect("/?return=" .. ngx.escape_uri(target), 302)
    end

    local code = rand_hex(32)
    conn:query(
        "INSERT INTO oidc_codes (code, client_id, user_id, redirect_uri, " ..
        "nonce, scope, code_challenge, code_challenge_method, expires_at) " ..
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        code, a.client_id, user.id, a.redirect_uri, a.nonce, a.scope,
        a.code_challenge, a.code_challenge_method, ngx.time() + CODE_TTL)
    conn:close()

    local sep = a.redirect_uri:find("?", 1, true) and "&" or "?"
    local loc = a.redirect_uri .. sep .. "code=" .. ngx.escape_uri(code)
    if a.state then loc = loc .. "&state=" .. ngx.escape_uri(a.state) end
    return ngx.redirect(loc, 302)
end

-- Verify a PKCE code_verifier against the stored challenge.
local function pkce_ok(method, challenge, verifier)
    if not challenge then return true end          -- PKCE not used
    if not verifier then return false end
    if method == "plain" then return verifier == challenge end
    if method == "S256" then
        return jwt.sha256_b64url(verifier) == challenge
    end
    return false
end

-- POST /api/oidc/token  (application/x-www-form-urlencoded)
function _M.token()
    ngx.req.read_body()
    local f = ngx.req.get_post_args()
    if f.grant_type ~= "authorization_code" then
        json.response({ error = "unsupported_grant_type" }, 400)
        return
    end

    local conn = open_db()
    local row = conn:query_one(
        "SELECT * FROM oidc_codes WHERE code = ?", f.code)
    if not row or row.expires_at < ngx.time() then
        if row then conn:query("DELETE FROM oidc_codes WHERE code = ?", f.code) end
        conn:close()
        json.response({ error = "invalid_grant" }, 400)
        return
    end
    -- One-time use.
    conn:query("DELETE FROM oidc_codes WHERE code = ?", f.code)

    if f.client_id ~= row.client_id or f.redirect_uri ~= row.redirect_uri then
        conn:close()
        json.response({ error = "invalid_grant" }, 400)
        return
    end

    local client = conn:query_one(
        "SELECT client_secret_hash FROM oidc_clients WHERE client_id = ?",
        row.client_id)
    if not client or not f.client_secret
        or not argon2.verify(client.client_secret_hash, f.client_secret) then
        conn:close()
        json.response({ error = "invalid_client" }, 401)
        return
    end

    if not pkce_ok(row.code_challenge_method, row.code_challenge, f.code_verifier) then
        conn:close()
        json.response({ error = "invalid_grant", detail = "pkce" }, 400)
        return
    end

    local user = conn:query_one(
        "SELECT id, username, email, email_verified, is_admin " ..
        "FROM users WHERE id = ?", row.user_id)
    if not user then
        conn:close()
        json.response({ error = "invalid_grant" }, 400)
        return
    end

    local scope = row.scope or "openid"
    local claims = build_claims(conn, user, row.client_id, scope)
    if row.nonce then claims.nonce = row.nonce end
    conn:close()

    local kid = keys.kid()
    local id_token, err = jwt.sign(claims, keys.key_path(), { kid = kid })
    if not id_token then
        json.response({ error = "server_error", detail = err }, 500)
        return
    end
    -- access_token is a JWT too, so userinfo is stateless.
    local at_claims = {
        iss = issuer(), sub = tostring(user.id), aud = row.client_id,
        iat = ngx.time(), exp = ngx.time() + TOKEN_TTL, scope = scope,
    }
    local access_token = jwt.sign(at_claims, keys.key_path(), { kid = kid })

    json.response({
        access_token = access_token,
        token_type = "Bearer",
        expires_in = TOKEN_TTL,
        id_token = id_token,
        scope = scope,
    })
end

-- GET /api/oidc/userinfo  (Authorization: Bearer <access_token>)
function _M.userinfo()
    local hdr = ngx.var.http_authorization
    local token = hdr and hdr:match("^Bearer%s+(.+)$")
    if not token then
        json.response({ error = "invalid_token" }, 401)
        return
    end
    local claims, err = jwt.verify(token, keys.pub_path())
    if not claims then
        json.response({ error = "invalid_token", detail = err }, 401)
        return
    end

    local conn = open_db()
    local user = conn:query_one(
        "SELECT id, username, email, email_verified, is_admin " ..
        "FROM users WHERE id = ?", tonumber(claims.sub))
    if not user then
        conn:close()
        json.response({ error = "invalid_token" }, 401)
        return
    end
    local info = build_claims(conn, user, claims.aud, claims.scope or "")
    conn:close()
    -- userinfo omits the token framing fields.
    info.aud, info.iss, info.iat, info.exp = nil, nil, nil, nil
    json.response(info)
end

-- ---- Client registration (admin) -------------------------------------

local function require_admin()
    local user = ngx.ctx.user
    if not user or user.is_admin ~= 1 then
        json.response({ error = "forbidden" }, 403)
        return nil
    end
    return user
end

-- POST /api/oidc/clients  { name, redirect_uris:[..], vm_id? }
-- Returns the client_secret ONCE (only the hash is stored).
function _M.create_client(body)
    if not require_admin() then return end
    if not body or not body.name or type(body.redirect_uris) ~= "table"
        or #body.redirect_uris == 0 then
        json.response({ error = "name and redirect_uris[] required" }, 400)
        return
    end
    local client_id = "flynas-" .. rand_hex(8)
    local secret = ngx.encode_base64(rand_hex(16))
    local hash, herr = argon2.hash(secret)
    if not hash then
        json.response({ error = "hash failed: " .. (herr or "?") }, 500)
        return
    end

    local conn = open_db()
    local rows, derr = conn:query(
        "INSERT INTO oidc_clients (vm_id, name, client_id, " ..
        "client_secret_hash, redirect_uris) VALUES (?, ?, ?, ?, ?) RETURNING id",
        body.vm_id, body.name, client_id, hash,
        table.concat(body.redirect_uris, "\n"))
    conn:close()
    if not rows then
        json.response({ error = "insert failed: " .. (derr or "?") }, 500)
        return
    end
    json.response({
        id = rows[1].id,
        name = body.name,
        client_id = client_id,
        client_secret = secret,        -- shown once
        issuer = issuer(),
    }, 201)
end

-- GET /api/oidc/clients
function _M.list_clients()
    if not require_admin() then return end
    local conn = open_db()
    local rows = conn:query(
        "SELECT id, vm_id, name, client_id, redirect_uris, created_at " ..
        "FROM oidc_clients ORDER BY name") or {}
    conn:close()
    json.response({ clients = rows })
end

-- DELETE /api/oidc/clients/:id
function _M.delete_client(id)
    if not require_admin() then return end
    local conn = open_db()
    conn:query("DELETE FROM oidc_clients WHERE id = ?", id)
    conn:close()
    json.response({ deleted = id })
end

return _M
