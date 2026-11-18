-- The game's LUI elements (menus, roots, timers) for the Lua tests, as the game hands them over: userdata, not Lua
-- tables, whose methods and fields are reached through their metatable. Stand-ins made of tables let checkpoint.lua's
-- `type(menu) == "table"` tests pass here while they failed in every game (the pad's quick save never saved, and the
-- pause was never seen; found 2026-11-18), so every stand-in here is a real userdata: a handle on /dev/null, the only
-- userdata plain Lua can make. The handles' shared metatable sends the ones made here to a table of their own fields
-- and methods, and every other file handle on to the file methods as before.
--     local element = dofile("tests/lui_element.lua")
--     local menu = element{ getParent = function() return root end }
--     menu.getParent = function() return nil end -- fields are set as on the game's elements
-- type(menu) is "userdata", and rawget, pairs or # on it fail, as they do on the game's.
local meta = getmetatable(io.stdout)
local elements = rawget(meta, "BO3MacFixElements")
if elements == nil then
    elements = setmetatable({}, { __mode = "k" })
    local file_index = meta.__index
    meta.__index = function(handle, key)
        local fields = elements[handle]
        if fields ~= nil then return fields[key] end
        if type(file_index) == "function" then return file_index(handle, key) end
        return file_index[key]
    end
    meta.__newindex = function(handle, key, value)
        local fields = elements[handle]
        if fields == nil then error("a file handle takes no fields", 2) end
        fields[key] = value
    end
    rawset(meta, "BO3MacFixElements", elements)
end

return function(fields)
    local handle = assert(io.open("/dev/null", "r"))
    elements[handle] = fields or {}
    return handle
end
