local _M = {}

local HELPER = "/usr/local/flynas/bin/flynas-helper"

local function run(args)
    local cmd = HELPER
    for _, arg in ipairs(args) do
        cmd = cmd .. " " .. arg
    end
    cmd = cmd .. " 2>&1"

    local pipe = io.popen(cmd, "r")
    local output = pipe:read("*a")
    local ok, _, code = pipe:close()

    if ok then
        return true
    end
    return nil, output ~= "" and output or ("exit code " .. (code or "unknown"))
end

local function run_with_stdin(args, input)
    local cmd = "printf '%s' '" .. input:gsub("'", "'\\''") .. "' | " .. HELPER
    for _, arg in ipairs(args) do
        cmd = cmd .. " " .. arg
    end
    cmd = cmd .. " 2>&1"

    local pipe = io.popen(cmd, "r")
    local output = pipe:read("*a")
    local ok, _, code = pipe:close()

    if ok then
        return true
    end
    return nil, output ~= "" and output or ("exit code " .. (code or "unknown"))
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

function _M.write_ssh_keys(username, keys_text)
    return run_with_stdin({ "sshkeys", username, "write" }, keys_text)
end

function _M.remove_ssh_keys(username)
    return run({ "sshkeys", username, "remove" })
end

return _M
