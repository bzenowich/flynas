local json = require("util.json")
local db = require("util.db")
local exec = require("util.exec")

local DB_PATH = "/usr/local/flynas/flynas.db"

local _M = {}

local function valid_name(s)
    return s and #s >= 1 and #s <= 32 and s:match("^[a-z_][a-z0-9_%-]*$")
end

-- GET /api/groups
function _M.list()
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local rows = conn:query(
        "SELECT g.id, g.name, COUNT(ug.user_id) as member_count " ..
        "FROM groups g LEFT JOIN user_groups ug ON ug.group_id = g.id " ..
        "GROUP BY g.id, g.name"
    )
    conn:close()

    json.response(rows or {})
end

-- POST /api/groups
function _M.create(body)
    if not body or not body.name then
        json.response({ error = "name required" }, 400)
        return
    end

    if not valid_name(body.name) then
        json.response({ error = "invalid group name" }, 400)
        return
    end

    -- Create system group
    local ok, exec_err = exec.group_add(body.name)
    if not ok then
        ngx.log(ngx.ERR, "groupadd failed: ", exec_err)
        json.response({ error = "failed to create system group" }, 500)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local rows, db_err = conn:query(
        "INSERT INTO groups (name) VALUES (?) RETURNING id, name",
        body.name
    )
    conn:close()

    if not rows then
        ngx.log(ngx.ERR, "group insert failed: ", db_err)
        json.response({ error = "failed to create group" }, 500)
        return
    end

    json.response(rows[1], 201)
end

-- GET /api/groups/:id
function _M.get(group_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local group = conn:query_one("SELECT id, name FROM groups WHERE id = ?", group_id)
    if not group then
        conn:close()
        json.response({ error = "group not found" }, 404)
        return
    end

    local members = conn:query(
        "SELECT u.id, u.username FROM users u " ..
        "JOIN user_groups ug ON ug.user_id = u.id " ..
        "WHERE ug.group_id = ?",
        group_id
    )
    conn:close()

    group.members = members or {}
    json.response(group)
end

-- PUT /api/groups/:id
function _M.update(group_id, body)
    if not body or not body.name then
        json.response({ error = "name required" }, 400)
        return
    end

    if not valid_name(body.name) then
        json.response({ error = "invalid group name" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local group = conn:query_one("SELECT name FROM groups WHERE id = ?", group_id)
    if not group then
        conn:close()
        json.response({ error = "group not found" }, 404)
        return
    end

    -- Rename system group
    local ok, exec_err = exec.group_mod(group.name, body.name)
    if not ok then
        conn:close()
        ngx.log(ngx.ERR, "groupmod failed: ", exec_err)
        json.response({ error = "failed to rename system group" }, 500)
        return
    end

    conn:query("UPDATE groups SET name = ? WHERE id = ?", body.name, group_id)
    conn:close()

    json.response({ status = "ok" })
end

-- DELETE /api/groups/:id
function _M.delete(group_id)
    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local group = conn:query_one("SELECT name FROM groups WHERE id = ?", group_id)
    if not group then
        conn:close()
        json.response({ error = "group not found" }, 404)
        return
    end

    -- Delete system group
    local ok, exec_err = exec.group_del(group.name)
    if not ok then
        ngx.log(ngx.ERR, "groupdel failed: ", exec_err)
    end

    -- user_groups has no ON DELETE CASCADE; clear memberships first
    conn:query("DELETE FROM user_groups WHERE group_id = ?", group_id)
    local _, db_err = conn:query("DELETE FROM groups WHERE id = ?", group_id)
    conn:close()

    if db_err then
        ngx.log(ngx.ERR, "group delete failed: ", db_err)
        json.response({ error = "failed to delete group" }, 500)
        return
    end

    json.response({ status = "ok" })
end

-- PUT /api/groups/:id/members
function _M.set_members(group_id, body)
    if not body or not body.user_ids then
        json.response({ error = "user_ids required" }, 400)
        return
    end

    local conn, err = db.open(DB_PATH)
    if not conn then
        json.response({ error = "internal error" }, 500)
        return
    end

    local group = conn:query_one("SELECT name FROM groups WHERE id = ?", group_id)
    if not group then
        conn:close()
        json.response({ error = "group not found" }, 404)
        return
    end

    -- Clear existing memberships
    conn:query("DELETE FROM user_groups WHERE group_id = ?", group_id)

    -- Add new memberships and collect usernames for system sync
    local usernames = {}
    for _, uid in ipairs(body.user_ids) do
        local user = conn:query_one("SELECT username FROM users WHERE id = ?", uid)
        if user then
            conn:query(
                "INSERT INTO user_groups (user_id, group_id) VALUES (?, ?)",
                uid, group_id
            )
            usernames[#usernames + 1] = user.username
        end
    end

    conn:close()

    -- Sync system group membership
    if #usernames > 0 then
        exec.group_members(group.name, table.concat(usernames, ","))
    end

    json.response({ status = "ok" })
end

return _M
