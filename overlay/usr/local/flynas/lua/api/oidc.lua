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

-- Guest-facing issuer for installed app VMs: they reach FlyNAS over the
-- flynas0 NAT bridge (10.77.0.1, plain http — there's no cert for a guest to
-- trust). install stashes this and the app's recipe injects it as the app's
-- configured issuer, so it equals the request-relative issuer() the app sees
-- when it fetches discovery/token over the bridge. §2.11 #5.
local BRIDGE_ISSUER = "http://10.77.0.1/api/oidc"

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

-- Issuer/base URL derived from the request, so it's correct for whichever
-- host+scheme the client reached us on: https on the management port for a
-- browser, http on the 10.77.0.1 bridge for an app VM's server-to-server
-- calls (discovery/token/jwks/userinfo). The id_token `iss` and the discovery
-- `issuer` therefore always match what the client actually fetched. §2.11 #5.
local function issuer()
    local scheme = ngx.var.scheme or "https"
    return scheme .. "://" .. (ngx.var.http_host or "localhost") .. "/api/oidc"
end

-- Browser-facing base for the authorize endpoint. Apps redirect the user's
-- browser there, so it must be LAN-reachable — not the 10.77.0.1 bridge that
-- carries the app's own server-to-server calls. Admins set `external_url`
-- (e.g. https://nas.example.com) in config; unset ⇒ fall back to the
-- request-relative issuer (correct in dev where browser and app share a host).
local function authorize_base(conn)
    local row = conn:query_one(
        "SELECT value FROM config WHERE key = 'external_url'")
    if row and row.value and #row.value > 0 then
        return (row.value:gsub("/+$", "")) .. "/api/oidc"
    end
    return issuer()
end

-- Bare host (no scheme/port/path) the browser uses to reach FlyNAS and, via
-- port-forwards, its apps. From config.external_url, else the current request
-- Host (the admin's browser host at install time). §2.11 #5/#6.
function _M.external_host(conn)
    local row = conn:query_one(
        "SELECT value FROM config WHERE key = 'external_url'")
    local url = (row and row.value and #row.value > 0 and row.value)
        or ngx.var.http_host or "localhost"
    return (url:gsub("^%w+://", ""):gsub("/.*$", ""):gsub(":%d+$", ""))
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
    local conn = open_db()
    -- issuer/token/jwks/userinfo are request-relative (bridge when an app
    -- fetches them over 10.77.0.1); only authorize is browser-facing. §2.11 #5.
    local iss = issuer()
    local authz = authorize_base(conn) .. "/authorize"
    conn:close()
    json.response({
        issuer = iss,
        authorization_endpoint = authz,
        token_endpoint = iss .. "/token",
        userinfo_endpoint = iss .. "/userinfo",
        jwks_uri = iss .. "/jwks",
        response_types_supported = { "code" },
        grant_types_supported = { "authorization_code" },
        subject_types_supported = { "public" },
        id_token_signing_alg_values_supported = { "RS256" },
        scopes_supported = { "openid", "profile", "email", "groups" },
        token_endpoint_auth_methods_supported = { "client_secret_basic", "client_secret_post" },
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

    -- Per-app authorization. Admins always pass; everyone else needs an
    -- app_grant for this client. Denial follows the OIDC error-redirect
    -- convention (bounce back to the app with error=access_denied) rather
    -- than showing a FlyNAS page the app can't interpret.
    if user.is_admin ~= 1 then
        local grant = conn:query_one(
            "SELECT 1 FROM app_grants WHERE client_id = ? AND user_id = ?",
            client.id, user.id)
        if not grant then
            conn:close()
            local sep = a.redirect_uri:find("?", 1, true) and "&" or "?"
            local loc = a.redirect_uri .. sep .. "error=access_denied"
            if a.state then loc = loc .. "&state=" .. ngx.escape_uri(a.state) end
            return ngx.redirect(loc, 302)
        end
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

    -- Client authentication: accept client_secret_basic (Authorization: Basic
    -- <b64(client_id:client_secret)>) as well as client_secret_post (creds in
    -- the body). Basic is the OIDC default and what Forgejo (and most clients)
    -- send, so we must read it or every token exchange fails. §2.11 #7.
    local cid, csecret = f.client_id, f.client_secret
    local auth = ngx.var.http_authorization
    if auth then
        local b64 = auth:match("^%s*[Bb]asic%s+(%S+)")
        local dec = b64 and ngx.decode_base64(b64)
        if dec then
            local u, p = dec:match("^([^:]*):(.*)$")
            -- Per RFC 6749 §2.3.1 the two are form-urlencoded before base64.
            if u and u ~= "" then cid = cid or ngx.unescape_uri(u) end
            if p then csecret = csecret or ngx.unescape_uri(p) end
        end
    end

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

    if cid ~= row.client_id or f.redirect_uri ~= row.redirect_uri then
        conn:close()
        json.response({ error = "invalid_grant" }, 400)
        return
    end

    local client = conn:query_one(
        "SELECT client_secret_hash FROM oidc_clients WHERE client_id = ?",
        row.client_id)
    if not client or not csecret
        or not argon2.verify(client.client_secret_hash, csecret) then
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

-- Mint a client row: generate id + secret, hash, insert. Returns
-- (record, err) with record = { id, client_id, secret }. Shared by the
-- admin endpoint and the install-time auto-provisioner; takes a caller's
-- conn and writes no HTTP response of its own.
local function mint_client(conn, name, redirect_uris, vm_id)
    local client_id = "flynas-" .. rand_hex(8)
    local secret = ngx.encode_base64(rand_hex(16))
    local hash, herr = argon2.hash(secret)
    if not hash then return nil, "hash failed: " .. (herr or "?") end
    local rows, derr = conn:query(
        "INSERT INTO oidc_clients (vm_id, name, client_id, " ..
        "client_secret_hash, redirect_uris) VALUES (?, ?, ?, ?, ?) RETURNING id",
        vm_id, name, client_id, hash, table.concat(redirect_uris, "\n"))
    if not rows then return nil, "insert failed: " .. (derr or "?") end
    return { id = rows[1].id, client_id = client_id, secret = secret }, nil
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

    local conn = open_db()
    local rec, err = mint_client(conn, body.name, body.redirect_uris, body.vm_id)
    conn:close()
    if not rec then
        json.response({ error = err }, 500)
        return
    end
    json.response({
        id = rec.id,
        name = body.name,
        client_id = rec.client_id,
        client_secret = rec.secret,    -- shown once
        issuer = issuer(),
    }, 201)
end

-- Auto-mint a VM-scoped client at app install and stash it for guest
-- provisioning. Called from apps.install with that handler's `conn`;
-- best-effort, so it returns (summary, err) instead of writing a response —
-- the caller logs err and still completes the install. The plaintext secret
-- is held in app_provisioning until the guest provisioning step delivers it.
-- Returns nil,nil when the template isn't OIDC-capable (no redirect path).
function _M.provision_for_vm(conn, vm, tpl, app_base)
    if not tpl.oidc_redirect_path then return nil, nil end
    if not vm.ip_address then return nil, "no guest IP yet" end
    -- app_base is the app's browser-facing base (external host + port-forward),
    -- passed by apps.install once the forward exists. Falls back to the guest
    -- bridge IP:port — reachable only where the browser shares the bridge (dev).
    local base = app_base or string.format("http://%s:%d",
        vm.ip_address, tpl.monitor_port or 80)
    local redirect_uri = base .. tpl.oidc_redirect_path
    local rec, err = mint_client(conn, vm.name .. "-sso", { redirect_uri }, vm.id)
    if not rec then return nil, err end
    -- Stash the *bridge* issuer, not issuer() — install runs in the admin's
    -- https request, but the value we bake into the guest must be the URL the
    -- guest itself can reach (10.77.0.1 over http). §2.11 #5.
    local iss = BRIDGE_ISSUER
    conn:query(
        "INSERT OR REPLACE INTO app_provisioning " ..
        "(vm_id, oidc_client_id, oidc_secret, redirect_uri, issuer, delivered) " ..
        "VALUES (?, ?, ?, ?, ?, 0)",
        vm.id, rec.id, rec.secret, redirect_uri, iss)
    -- Summary omits the secret: that travels to the guest, not the API caller.
    return { client_id = rec.client_id, redirect_uri = redirect_uri,
             issuer = iss, pending = true }, nil
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

-- ---- App access grants (admin) ---------------------------------------
-- Grants gate /authorize: a non-admin with no grant for a client cannot
-- log into that app (admins always can). The grant's role also feeds the
-- `groups` claim ("admin" vs "user") via build_claims.

-- GET /api/oidc/clients/:id/grants → who may access this app + their role
function _M.list_grants(client_id)
    if not require_admin() then return end
    local conn = open_db()
    local rows = conn:query(
        "SELECT ag.user_id, u.username, ag.role FROM app_grants ag " ..
        "JOIN users u ON u.id = ag.user_id WHERE ag.client_id = ? " ..
        "ORDER BY u.username", client_id) or {}
    conn:close()
    json.response({ grants = rows })
end

-- PUT /api/oidc/clients/:id/grants  { grants: [{ user_id, role }] }
-- Replaces the client's full grant set (mirrors groups.set_members).
function _M.set_grants(client_id, body)
    if not require_admin() then return end
    if not body or type(body.grants) ~= "table" then
        json.response({ error = "grants[] required" }, 400)
        return
    end
    for _, g in ipairs(body.grants) do
        if type(g.user_id) ~= "number" then
            json.response({ error = "each grant needs a numeric user_id" }, 400)
            return
        end
        if g.role and g.role ~= "user" and g.role ~= "admin" then
            json.response({ error = "role must be 'user' or 'admin'" }, 400)
            return
        end
    end

    local conn = open_db()
    local client = conn:query_one(
        "SELECT id FROM oidc_clients WHERE id = ?", client_id)
    if not client then
        conn:close()
        json.response({ error = "invalid_client" }, 404)
        return
    end

    conn:query("DELETE FROM app_grants WHERE client_id = ?", client_id)
    local count = 0
    for _, g in ipairs(body.grants) do
        -- Skip unknown users rather than failing the whole set.
        local u = conn:query_one("SELECT id FROM users WHERE id = ?", g.user_id)
        if u then
            conn:query(
                "INSERT INTO app_grants (user_id, client_id, role) " ..
                "VALUES (?, ?, ?)", g.user_id, client_id, g.role or "user")
            count = count + 1
        end
    end
    conn:close()
    json.response({ client_id = client_id, count = count })
end

return _M
