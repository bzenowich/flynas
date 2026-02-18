local ffi = require("ffi")

ffi.cdef[[
    typedef enum Argon2_type {
        Argon2_d = 0,
        Argon2_i = 1,
        Argon2_id = 2
    } argon2_type;

    int argon2id_hash_encoded(
        uint32_t t_cost, uint32_t m_cost, uint32_t parallelism,
        const void *pwd, size_t pwdlen,
        const void *salt, size_t saltlen,
        size_t hashlen,
        char *encoded, size_t encodedlen
    );

    int argon2id_verify(
        const char *encoded,
        const void *pwd, size_t pwdlen
    );

    size_t argon2_encodedlen(
        uint32_t t_cost, uint32_t m_cost,
        uint32_t parallelism, uint32_t saltlen,
        uint32_t hashlen, argon2_type type
    );
]]

local lib = ffi.load("argon2")

local ARGON2_OK = 0
local T_COST = 3
local M_COST = 65536  -- 64 MB
local PARALLELISM = 1
local HASH_LEN = 32
local SALT_LEN = 16

local _M = {}

local function random_bytes(n)
    local f = io.open("/dev/urandom", "rb")
    local bytes = f:read(n)
    f:close()
    return bytes
end

function _M.hash(password)
    local salt = random_bytes(SALT_LEN)
    local encoded_len = lib.argon2_encodedlen(
        T_COST, M_COST, PARALLELISM, SALT_LEN, HASH_LEN, ffi.C.Argon2_id
    )
    local buf = ffi.new("char[?]", encoded_len)

    local rc = lib.argon2id_hash_encoded(
        T_COST, M_COST, PARALLELISM,
        password, #password,
        salt, SALT_LEN,
        HASH_LEN,
        buf, encoded_len
    )

    if rc ~= ARGON2_OK then
        return nil, "argon2 hash failed: " .. rc
    end

    return ffi.string(buf)
end

function _M.verify(encoded, password)
    local rc = lib.argon2id_verify(encoded, password, #password)
    return rc == ARGON2_OK
end

return _M
