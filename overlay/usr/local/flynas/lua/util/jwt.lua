-- Minimal JWT (RS256) for the OIDC provider. Signing/verification shell
-- out to base openssl — no privilege needed, so this runs directly via
-- io.popen (like util/exec.run_shell), not through the setuid helper.
--
-- The signing input is base64url (URL-safe charset, no '=' padding) of
-- the JSON header + payload, which contains no shell metacharacters, so
-- piping it through printf is safe.
local cjson = require("cjson")

local _M = {}

-- base64url encode a (possibly binary) string
function _M.b64url_encode(s)
    local b64 = ngx.encode_base64(s, true)  -- true = no padding
    return (b64:gsub("%+", "-"):gsub("/", "_"))
end

-- base64url decode back to bytes
function _M.b64url_decode(s)
    s = s:gsub("%-", "+"):gsub("_", "/")
    local pad = #s % 4
    if pad > 0 then
        s = s .. string.rep("=", 4 - pad)
    end
    return ngx.decode_base64(s)
end

-- openssl is one-directional through io.popen, so stage stdin via a temp
-- file under the private run dir and read stdout back.
local TMP_DIR = "/usr/local/flynas/run"

local function rand_suffix()
    local f = io.open("/dev/urandom", "rb")
    local b = f:read(8)
    f:close()
    local h = {}
    for i = 1, #b do h[i] = string.format("%02x", string.byte(b, i)) end
    return table.concat(h)
end

-- SHA-256 of a string, base64url-encoded. Used for PKCE S256. openssl
-- rather than resty.sha256, which this OpenResty build doesn't bundle.
function _M.sha256_b64url(s)
    local in_path = TMP_DIR .. "/sha-" .. rand_suffix()
    local f = io.open(in_path, "wb")
    if not f then return nil, "cannot write temp input" end
    f:write(s)
    f:close()

    local p = io.popen(string.format(
        "openssl dgst -sha256 -binary %q", in_path), "r")
    local raw = p and p:read("*a") or nil
    if p then p:close() end
    os.remove(in_path)

    if not raw or #raw == 0 then return nil, "openssl sha256 failed" end
    return _M.b64url_encode(raw)
end

-- Sign `signing_input` (a string) with the RSA private key at key_path,
-- returning the raw signature bytes.
local function rsa_sign(signing_input, key_path)
    local base = TMP_DIR .. "/jwt-" .. rand_suffix()
    local in_path, sig_path = base .. ".in", base .. ".sig"

    local f = io.open(in_path, "wb")
    if not f then return nil, "cannot write temp input" end
    f:write(signing_input)
    f:close()

    local cmd = string.format(
        "openssl dgst -sha256 -sign %q -out %q %q 2>&1",
        key_path, sig_path, in_path)
    local p = io.popen(cmd, "r")
    local err_out = p and p:read("*a") or ""
    if p then p:close() end

    local sf = io.open(sig_path, "rb")
    local sig = sf and sf:read("*a") or nil
    if sf then sf:close() end

    os.remove(in_path)
    os.remove(sig_path)

    if not sig or #sig == 0 then
        return nil, "openssl sign failed: " .. (err_out or "?")
    end
    return sig
end

-- Verify `signing_input` against `sig` (raw bytes) using the public key
-- (PEM at pub_path). Returns true/false.
local function rsa_verify(signing_input, sig, pub_path)
    local base = TMP_DIR .. "/jwt-" .. rand_suffix()
    local in_path, sig_path = base .. ".in", base .. ".sig"

    local f = io.open(in_path, "wb"); f:write(signing_input); f:close()
    local sf = io.open(sig_path, "wb"); sf:write(sig); sf:close()

    local cmd = string.format(
        "openssl dgst -sha256 -verify %q -signature %q %q 2>&1",
        pub_path, sig_path, in_path)
    local p = io.popen(cmd, "r")
    local out = p and p:read("*a") or ""
    if p then p:close() end

    os.remove(in_path)
    os.remove(sig_path)
    return out:match("Verified OK") ~= nil
end

-- Sign a claims table into a compact JWT. opts.kid sets the header kid.
function _M.sign(claims, key_path, opts)
    opts = opts or {}
    local header = { alg = "RS256", typ = "JWT", kid = opts.kid }
    local signing_input =
        _M.b64url_encode(cjson.encode(header)) .. "." ..
        _M.b64url_encode(cjson.encode(claims))

    local sig, err = rsa_sign(signing_input, key_path)
    if not sig then return nil, err end
    return signing_input .. "." .. _M.b64url_encode(sig)
end

-- Verify a compact JWT against pub_path. Returns (claims, nil) or
-- (nil, err). Checks signature and `exp` (if present).
function _M.verify(token, pub_path)
    local h, p, s = token:match("^([^%.]+)%.([^%.]+)%.([^%.]+)$")
    if not h then return nil, "malformed token" end

    local sig = _M.b64url_decode(s)
    if not sig or not rsa_verify(h .. "." .. p, sig, pub_path) then
        return nil, "bad signature"
    end

    local ok, claims = pcall(cjson.decode, _M.b64url_decode(p))
    if not ok then return nil, "bad payload" end

    if claims.exp and tonumber(claims.exp) and ngx.time() >= claims.exp then
        return nil, "token expired"
    end
    return claims
end

return _M
