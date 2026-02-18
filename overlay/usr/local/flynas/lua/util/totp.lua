local bit = require("bit")
local ffi = require("ffi")

local _M = {}

local b32_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
local b32_lookup = {}
for i = 1, #b32_alphabet do
    b32_lookup[b32_alphabet:byte(i)] = i - 1
end

function _M.base32_decode(input)
    input = input:upper():gsub("[= ]", "")
    local out = {}
    local buffer = 0
    local bits = 0

    for i = 1, #input do
        local val = b32_lookup[input:byte(i)]
        if not val then
            return nil, "invalid base32 character"
        end
        buffer = bit.bor(bit.lshift(buffer, 5), val)
        bits = bits + 5
        if bits >= 8 then
            bits = bits - 8
            out[#out + 1] = string.char(bit.band(bit.rshift(buffer, bits), 0xFF))
        end
    end

    return table.concat(out)
end

function _M.base32_encode(input)
    local out = {}
    local buffer = 0
    local bits = 0

    for i = 1, #input do
        buffer = bit.bor(bit.lshift(buffer, 8), input:byte(i))
        bits = bits + 8
        while bits >= 5 do
            bits = bits - 5
            local idx = bit.band(bit.rshift(buffer, bits), 0x1F)
            out[#out + 1] = b32_alphabet:sub(idx + 1, idx + 1)
        end
    end

    if bits > 0 then
        local idx = bit.band(bit.lshift(buffer, 5 - bits), 0x1F)
        out[#out + 1] = b32_alphabet:sub(idx + 1, idx + 1)
    end

    -- Pad to multiple of 8
    while #out % 8 ~= 0 do
        out[#out + 1] = "="
    end

    return table.concat(out)
end

function _M.generate_secret()
    local f = io.open("/dev/urandom", "rb")
    local bytes = f:read(20)
    f:close()
    return _M.base32_encode(bytes)
end

-- HOTP: RFC 4226
local function hotp(key_bytes, counter)
    -- Encode counter as 8-byte big-endian
    local counter_bytes = {}
    for i = 8, 1, -1 do
        counter_bytes[i] = string.char(bit.band(counter, 0xFF))
        counter = math.floor(counter / 256)
    end
    local counter_str = table.concat(counter_bytes)

    local hmac = ngx.hmac_sha1(key_bytes, counter_str)

    -- Dynamic truncation
    local offset = bit.band(hmac:byte(20), 0x0F)
    local code = bit.bor(
        bit.lshift(bit.band(hmac:byte(offset + 1), 0x7F), 24),
        bit.lshift(hmac:byte(offset + 2), 16),
        bit.lshift(hmac:byte(offset + 3), 8),
        hmac:byte(offset + 4)
    )

    return string.format("%06d", code % 1000000)
end

function _M.generate(secret_b32, time)
    local key, err = _M.base32_decode(secret_b32)
    if not key then
        return nil, err
    end
    time = time or ngx.time()
    local counter = math.floor(time / 30)
    return hotp(key, counter)
end

function _M.verify(secret_b32, code, window)
    window = window or 1
    local key, err = _M.base32_decode(secret_b32)
    if not key then
        return false
    end

    local now = ngx.time()
    local counter = math.floor(now / 30)

    for i = -window, window do
        local expected = hotp(key, counter + i)
        if expected == code then
            return true
        end
    end

    return false
end

function _M.uri(issuer, account, secret_b32)
    local label = ngx.escape_uri(issuer) .. ":" .. ngx.escape_uri(account)
    return string.format(
        "otpauth://totp/%s?secret=%s&issuer=%s&digits=6&period=30",
        label, secret_b32, ngx.escape_uri(issuer)
    )
end

return _M
