-- Minimal QMP (QEMU Machine Protocol) client over the per-VM unix
-- socket. The socket is root:flynas 0750, so nginx (www, in flynas)
-- can drive running/suspend/resume/powerdown without the setuid
-- helper. Privileged ops (launch, force-kill, tap) stay in the helper.
local json = require("util.json")

local _M = {}

local RUN_DIR = "/var/run/flynas"

local function sock_addr(name)
    return "unix:" .. RUN_DIR .. "/vm-" .. name .. ".sock"
end

-- Read JSON lines until one carries a return/error (skipping events).
local function read_reply(sock)
    for _ = 1, 30 do
        local line, err = sock:receive("*l")
        if not line then return nil, err or "closed" end
        local ok, obj = pcall(json.decode, line)
        if ok and type(obj) == "table" and
           (obj["return"] ~= nil or obj.error ~= nil) then
            return obj
        end
    end
    return nil, "no reply"
end

-- Connect and complete the capabilities handshake.
local function connect(name)
    local sock = ngx.socket.tcp()
    sock:settimeout(3000)
    local ok, err = sock:connect(sock_addr(name))
    if not ok then return nil, err end
    -- server greeting
    local greet = sock:receive("*l")
    if not greet then sock:close(); return nil, "no greeting" end
    sock:send('{"execute":"qmp_capabilities"}\r\n')
    local reply, rerr = read_reply(sock)
    if not reply then sock:close(); return nil, rerr end
    return sock
end

local function command(name, execute)
    local sock, err = connect(name)
    if not sock then return nil, err end
    sock:send('{"execute":"' .. execute .. '"}\r\n')
    local reply, rerr = read_reply(sock)
    sock:close()
    if not reply then return nil, rerr end
    if reply.error then return nil, reply.error.desc or "qmp error" end
    return reply["return"] or true
end

-- "running" | "paused" | ... ; nil if the VM isn't reachable
function _M.status(name)
    local sock, err = connect(name)
    if not sock then return nil, err end
    sock:send('{"execute":"query-status"}\r\n')
    local reply, rerr = read_reply(sock)
    sock:close()
    if not reply or not reply["return"] then return nil, rerr or "no status" end
    return reply["return"].status
end

function _M.suspend(name) return command(name, "stop") end       -- pause CPUs
function _M.resume(name) return command(name, "cont") end        -- resume CPUs
function _M.powerdown(name) return command(name, "system_powerdown") end -- ACPI

return _M
