local json = require("util.json")

local _M = {}

-- Run a read-only shell command, return stdout or nil
local function capture(cmd)
    local pipe = io.popen(cmd .. " 2>/dev/null", "r")
    if not pipe then
        return nil
    end
    local output = pipe:read("*a")
    pipe:close()
    if output == "" then
        return nil
    end
    return output
end

local function sysctl(name)
    local out = capture("/sbin/sysctl -n " .. name)
    if out then
        return (out:gsub("%s+$", ""))
    end
    return nil
end

-- GET /api/dashboard/system
function _M.system()
    local boottime
    local bt = sysctl("kern.boottime")
    if bt then
        -- Format: { sec = 1748850000, usec = 123456 } ...
        boottime = tonumber(bt:match("sec%s*=%s*(%d+)"))
    end

    local loadavg = {}
    local la = sysctl("vm.loadavg")
    if la then
        -- Format: { 0.12 0.08 0.05 }
        for n in la:gmatch("[%d%.]+") do
            loadavg[#loadavg + 1] = tonumber(n)
        end
    end

    json.response({
        hostname = sysctl("kern.hostname"),
        os = sysctl("kern.ostype"),
        os_release = sysctl("kern.osrelease"),
        version = sysctl("kern.version"),
        boottime = boottime,
        uptime_sec = boottime and (ngx.time() - boottime) or nil,
        loadavg = loadavg,
    })
end

-- GET /api/dashboard/cpu
-- Returns raw cp_time ticks; the UI computes usage from deltas
-- between polls (avoids blocking the worker to sample twice).
function _M.cpu()
    local ticks = {}
    local cp = sysctl("kern.cp_time")
    if cp then
        for n in cp:gmatch("%d+") do
            ticks[#ticks + 1] = tonumber(n)
        end
    end

    json.response({
        ncpu = tonumber(sysctl("hw.ncpu")),
        -- user, nice, sys, intr, idle
        cp_time = {
            user = ticks[1],
            nice = ticks[2],
            sys  = ticks[3],
            intr = ticks[4],
            idle = ticks[5],
        },
        stathz = tonumber((sysctl("kern.clockrate") or ""):match("stathz%s*=%s*(%d+)")),
    })
end

-- GET /api/dashboard/memory
function _M.memory()
    local page_size = tonumber(sysctl("vm.stats.vm.v_page_size")) or 4096
    local function pages(name)
        local v = tonumber(sysctl(name))
        return v and v * page_size or nil
    end

    json.response({
        physmem = tonumber(sysctl("hw.physmem")),
        free = pages("vm.stats.vm.v_free_count"),
        active = pages("vm.stats.vm.v_active_count"),
        inactive = pages("vm.stats.vm.v_inactive_count"),
        wired = pages("vm.stats.vm.v_wire_count"),
        cache = pages("vm.stats.vm.v_cache_count"),
    })
end

-- GET /api/dashboard/network
function _M.network()
    local out = capture("/sbin/ifconfig -a")
    if not out then
        json.response({ error = "ifconfig failed" }, 500)
        return
    end

    local interfaces = {}
    local cur

    for line in out:gmatch("[^\n]+") do
        local name, flags = line:match("^([%w%.]+):%s+flags=%d+<([^>]*)>")
        if name then
            cur = {
                name = name,
                up = flags:find("UP") ~= nil,
                addresses = {},
            }
            interfaces[#interfaces + 1] = cur
        elseif cur then
            local ether = line:match("^%s+ether%s+(%x+:%x+:%x+:%x+:%x+:%x+)")
            if ether then
                cur.mac = ether
            end
            local ip, mask = line:match("^%s+inet%s+([%d%.]+)%s+netmask%s+(%S+)")
            if ip then
                cur.addresses[#cur.addresses + 1] = { ip = ip, netmask = mask }
            end
            local media = line:match("^%s+media:%s+(.+)$")
            if media then
                cur.media = media
            end
            local status = line:match("^%s+status:%s+(%S+)")
            if status then
                cur.status = status
            end
        end
    end

    json.response({ interfaces = interfaces })
end

-- GET /api/dashboard/disks
function _M.disks()
    local list = sysctl("kern.disks")
    if not list then
        json.response({ error = "kern.disks unavailable" }, 500)
        return
    end

    local disks = {}
    for name in list:gmatch("%S+") do
        -- Skip CD/DVD and virtual devices (vnode, memory disks)
        if not (name:match("^cd%d") or name:match("^vn%d") or name:match("^md%d")) then
            local disk = { name = name, device = "/dev/" .. name }

            -- Size from geometry: "/dev/vbd1: 8322 cyl 16 hd 63 sec"
            local geom = capture("/usr/local/flynas/bin/flynas-helper diskinfo " .. name)
            if geom then
                local cyl, hd, sec = geom:match("(%d+)%s+cyl%s+(%d+)%s+hd%s+(%d+)%s+sec")
                if cyl then
                    disk.size_bytes = tonumber(cyl) * tonumber(hd) * tonumber(sec) * 512
                end
            end

            -- SMART health + serial (virtio disks report nothing; that's fine)
            local smart = capture("/usr/local/flynas/bin/flynas-helper smart " .. name)
            if smart then
                disk.serial = smart:match("Serial Number:%s+(%S+)")
                local health = smart:match("overall%-health self%-assessment test result:%s+(%S+)")
                disk.smart_health = health or "unknown"
            else
                disk.smart_health = "unavailable"
            end

            disks[#disks + 1] = disk
        end
    end

    json.response({ disks = disks })
end

-- GET /api/dashboard/volumes
function _M.volumes()
    local out = capture("/bin/df -k -t hammer2")
    local volumes = {}

    if out then
        for line in out:gmatch("[^\n]+") do
            -- Filesystem 1024-blocks Used Avail Capacity Mounted on
            local dev, total, used, avail, pct, mount =
                line:match("^(%S+)%s+(%d+)%s+(%d+)%s+(%d+)%s+(%d+)%%%s+(%S+)")
            if dev then
                volumes[#volumes + 1] = {
                    device = dev,
                    mountpoint = mount,
                    total_kb = tonumber(total),
                    used_kb = tonumber(used),
                    available_kb = tonumber(avail),
                    used_pct = tonumber(pct),
                }
            end
        end
    end

    json.response({ volumes = volumes })
end

return _M
