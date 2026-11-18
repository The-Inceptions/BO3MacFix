-- KeepCrashLogs' UI half (crash_log.c): a line in BO3MacFix's log whenever a menu opens or closes, by its name ("menu:
-- ZMGlobeMapSelect opened over ChangeMap", "menu: ChangeMap closed"). Every menu is a CoD.Menu, and the game's own
-- CoD.Menu.NewFromState and CoD.Menu.close (ui/t6/codmenu.lua) call the engine's RegisterOpenedMenu(menu, name) for
-- each menu made and UnregisterOpenedMenu(menu, name) for each one closed, however it was opened (an overlay, a popup,
-- NavigateToMenu, the root's own) or closed (GoBack, Close, a menu taking its place). All-around Enhancement wraps the
-- first one the same way. So wrapping the two sees every menu come and go and runs nothing per frame: ours runs first,
-- in a pcall, then the function that was there, with every argument and every result as before. "Over" is the newest
-- menu of this UI still open. The first menu made in each UI Lua state loads this (notices.m), before that menu is
-- made. menu_line(text, newest) prints text with the time and keeps the newest open menu for the minute's line; debug
-- (BO3MACFIX_CRASHLOGDEBUG=1) adds the menus open after each change.
local menu_line, debug = ...
local bo3macfix = rawget(_G, "BO3MacFix")
if type(menu_line) ~= "function" or type(bo3macfix) ~= "table" or rawget(bo3macfix, "CrashLogMenus") ~= nil then
    return
end
local register, unregister = rawget(_G, "RegisterOpenedMenu"), rawget(_G, "UnregisterOpenedMenu")
if register == nil or unregister == nil then
    menu_line("menus: this UI has no RegisterOpenedMenu, so its menus aren't logged", nil)
    return
end
bo3macfix.CrashLogMenus = true

-- this UI's menus made and not closed yet, oldest first: { id, name }. The menus themselves are in a table that
-- doesn't keep them alive, so one the game drops without closing it is never kept by this.
local open, MAX_OPEN = {}, 32
local menus, next_id = {}, 0
pcall(setmetatable, menus, { __mode = "v" })

local function remove_at(index)
    local count = #open
    menus[open[index].id] = nil
    for i = index, count - 1 do
        open[i] = open[i + 1]
    end
    open[count] = nil
end

local function newest()
    local top = open[#open]
    if top == nil then return nil end
    return top.name
end

local function say(text)
    if debug then
        local names = ""
        for i = 1, #open do
            names = names .. (i > 1 and " > " or "") .. open[i].name
        end
        text = text .. " [open: " .. names .. "]"
    end
    menu_line(text, newest())
end

local function add(menu, name)
    if #open >= MAX_OPEN then remove_at(1) end
    next_id = next_id + 1
    menus[next_id] = menu
    open[#open + 1] = { id = next_id, name = name }
end

local function opened(menu, name)
    name = tostring(name)
    local over = newest()
    add(menu, name)
    if over ~= nil then
        say("menu: " .. name .. " opened over " .. over)
    else
        say("menu: " .. name .. " opened")
    end
end

local function closed(menu, name)
    for i = #open, 1, -1 do
        if menu ~= nil and menus[open[i].id] == menu then
            remove_at(i)
            break
        end
    end
    say("menu: " .. tostring(name) .. " closed")
end

-- the menus open already: elements with a menuName right under a root
local function seed()
    local names = ""
    for _, root in pairs(LUI.roots) do
        local child, count = root:getFirstChild(), 0
        while child ~= nil and count < 64 do
            local name = child.menuName
            if name ~= nil and #open < MAX_OPEN then
                add(child, tostring(name))
                names = names .. (names == "" and "" or ", ") .. tostring(name)
            end
            child, count = child:getNextSibling(), count + 1
        end
    end
    return names
end
local ok, names = pcall(seed)

rawset(_G, "RegisterOpenedMenu", function(menu, name, ...)
    pcall(opened, menu, name)
    return register(menu, name, ...)
end)
rawset(_G, "UnregisterOpenedMenu", function(menu, name, ...)
    pcall(closed, menu, name)
    return unregister(menu, name, ...)
end)

if ok and names ~= "" then
    pcall(say, "menus: watching this UI's menus, open already: " .. names)
else
    pcall(say, "menus: watching this UI's menus")
end
