-- Cloud-init user-data templating for app VM guest provisioning (§2.11).
-- A per-app recipe (lua/recipes/<app>.yaml) is a cloud-config document with
-- {{KEY}} placeholders; render() fills them from the VM + OIDC stash. The
-- result is piped to `flynas-helper vmseed` (exec.vm_seed) to build the
-- NoCloud seed the guest consumes on first boot.

local _M = {}

-- Substitute {{KEY}} placeholders from vars. Whitespace inside the braces
-- is tolerated ({{ KEY }}). Returns nil + message if any placeholder is
-- left unresolved, so a half-rendered seed (e.g. a literal
-- {{CLIENT_SECRET}}) can never reach a guest.
function _M.render(tpl, vars)
    if not tpl then return nil, "no recipe" end
    local out = tpl:gsub("{{%s*([%w_]+)%s*}}", function(key)
        local v = vars[key]
        if v == nil then return nil end   -- leave intact; caught below
        return tostring(v)
    end)
    local missing = out:match("{{%s*([%w_]+)%s*}}")
    if missing then
        return nil, "unresolved placeholder: " .. missing
    end
    return out
end

return _M
