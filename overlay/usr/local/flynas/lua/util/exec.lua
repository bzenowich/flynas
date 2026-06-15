local _M = {}

local HELPER = "/usr/local/flynas/bin/flynas-helper"

-- This LuaJIT's pipe:close() returns true regardless of the child's
-- exit status, so the status is captured in-band via a trailing
-- EXIT:<code> marker instead.
local function run_shell(cmd)
    cmd = cmd .. ' 2>&1; printf "\\nEXIT:%d" "$?"'

    local pipe = io.popen(cmd, "r")
    if not pipe then
        return nil, "popen failed"
    end
    local output = pipe:read("*a")
    pipe:close()

    local code = tonumber(output:match("\nEXIT:(%d+)%s*$"))
    output = output:gsub("\n?EXIT:%d+%s*$", ""):gsub("%s+$", "")
    if code == 0 then
        return true
    end
    return nil, output ~= "" and output or ("exit code " .. (code or "unknown"))
end

local function run(args)
    local cmd = HELPER
    for _, arg in ipairs(args) do
        cmd = cmd .. " " .. arg
    end
    return run_shell(cmd)
end

local function run_with_stdin(args, input)
    local cmd = "printf '%s' '" .. input:gsub("'", "'\\''") .. "' | " .. HELPER
    for _, arg in ipairs(args) do
        cmd = cmd .. " " .. arg
    end
    return run_shell(cmd)
end

function _M.user_add(username, shell)
    return run({ "useradd", username, shell or "/bin/sh" })
end

function _M.user_del(username)
    return run({ "userdel", username })
end

function _M.user_mod(username, opts)
    if opts.shell then
        local ok, err = run({ "usermod", username, "shell", opts.shell })
        if not ok then return nil, err end
    end
    if opts.groups then
        local ok, err = run({ "usermod", username, "groups", opts.groups })
        if not ok then return nil, err end
    end
    return true
end

function _M.set_password(username, password)
    return run_with_stdin({ "passwd", username }, password)
end

function _M.group_add(name)
    return run({ "groupadd", name })
end

function _M.group_del(name)
    return run({ "groupdel", name })
end

function _M.group_mod(old_name, new_name)
    return run({ "groupmod", old_name, new_name })
end

function _M.group_members(name, usernames)
    return run({ "groupmembers", name, usernames })
end

function _M.vol_create(label, disks)
    return run({ "volcreate", label, disks })
end

function _M.vol_destroy(label)
    return run({ "voldestroy", label })
end

function _M.vol_scrub(label)
    return run({ "scrub", label })
end

function _M.snap_create(label, snapname)
    return run({ "snapcreate", label, snapname })
end

function _M.snap_delete(label, snapname)
    return run({ "snapdelete", label, snapname })
end

function _M.backup_sync()
    return run({ "backupsync" })
end

function _M.net_dhcp(iface)
    return run({ "netconfig", iface, "dhcp" })
end

function _M.net_static(iface, ip, netmask, gateway)
    return run({ "netconfig", iface, "static", ip, netmask, gateway })
end

function _M.set_timezone(zone)
    return run({ "timezone", zone })
end

-- server = nil disables NTP
function _M.set_ntp(server)
    return run({ "ntp", server or "off" })
end

-- VMs (QEMU/NVMM). Paths are validated server-side and contain no
-- spaces, so the unquoted run() join is safe.
function _M.vm_create(name, gb, volume)
    return run({ "vmcreate", name, tostring(gb), volume or "-" })
end

function _M.vm_start(name, cpus, ram_mb, image, tap, mac, iso)
    return run({ "vmstart", name, tostring(cpus), tostring(ram_mb),
        image, tap, mac or "-", iso or "-" })
end

function _M.vm_stop(name, tap)
    return run({ "vmstop", name, tap })
end

function _M.vm_delete(name, volume)
    return run({ "vmdelete", name, volume or "-" })
end

function _M.vmbridge_up(uplink)
    return run({ "netbridge", "up", uplink })
end

function _M.vmbridge_down()
    return run({ "netbridge", "down" })
end

function _M.write_ssh_keys(username, keys_text)
    return run_with_stdin({ "sshkeys", username, "write" }, keys_text)
end

function _M.remove_ssh_keys(username)
    return run({ "sshkeys", username, "remove" })
end

return _M
