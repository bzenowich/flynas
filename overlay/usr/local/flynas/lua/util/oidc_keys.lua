-- RSA signing key for the OIDC provider. Lazily generated on first use
-- via base openssl, then cached on disk. Exposes the public half as a JWK
-- for the /api/oidc/jwks endpoint.
local jwt = require("util.jwt")

local _M = {}

local DIR = "/usr/local/flynas"
local KEY_PATH = DIR .. "/oidc_key.pem"
local PUB_PATH = DIR .. "/oidc_pub.pem"
local RUN_DIR = DIR .. "/run"

local function sh(cmd)
    local p = io.popen(cmd .. " 2>&1", "r")
    local out = p and p:read("*a") or ""
    if p then p:close() end
    return out
end

local function file_exists(path)
    local f = io.open(path, "rb")
    if f then f:close() return true end
    return false
end

-- Generate the keypair if absent. Idempotent; safe to call per request.
function _M.ensure()
    sh(string.format("mkdir -p %q", RUN_DIR))
    if file_exists(KEY_PATH) and file_exists(PUB_PATH) then
        return true
    end
    sh(string.format("openssl genrsa -out %q 2048", KEY_PATH))
    sh(string.format("chmod 600 %q", KEY_PATH))
    sh(string.format("openssl rsa -in %q -pubout -out %q", KEY_PATH, PUB_PATH))
    if not (file_exists(KEY_PATH) and file_exists(PUB_PATH)) then
        return nil, "oidc key generation failed"
    end
    return true
end

function _M.key_path() return KEY_PATH end
function _M.pub_path() return PUB_PATH end

-- Decode a hex string to raw bytes.
local function hex_to_bytes(hex)
    return (hex:gsub("%x%x", function(cc)
        return string.char(tonumber(cc, 16))
    end))
end

-- Key id: stable per key, derived from the modulus.
function _M.kid()
    local ok = _M.ensure()
    if not ok then return "flynas" end
    local out = sh(string.format("openssl rsa -in %q -noout -modulus", KEY_PATH))
    local hex = out:match("Modulus=(%x+)")
    return hex and hex:sub(1, 16):lower() or "flynas"
end

-- Public key as a single-entry JWK set.
function _M.jwks()
    local ok, err = _M.ensure()
    if not ok then return nil, err end
    local out = sh(string.format("openssl rsa -in %q -noout -modulus", KEY_PATH))
    local hex = out:match("Modulus=(%x+)")
    if not hex then return nil, "could not read modulus" end
    local n = jwt.b64url_encode(hex_to_bytes(hex))
    return {
        keys = {
            {
                kty = "RSA",
                use = "sig",
                alg = "RS256",
                kid = hex:sub(1, 16):lower(),
                n = n,
                e = "AQAB",  -- genrsa default public exponent (65537)
            },
        },
    }
end

return _M
