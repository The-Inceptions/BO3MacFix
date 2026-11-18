-- Runs source/crash_log.lua against a made-up CoD.Menu and engine, with any Lua 5.x: lua tests/test_crash_log.lua
-- Checks the lines for menus opening and closing and what they are opened over, that the engine's RegisterOpenedMenu
-- and UnregisterOpenedMenu still get every argument and hand back every result, also when the line fails, that a
-- wrapper of the mod's put on top before or after chains with ours, and that loading it twice changes nothing.
local failures = 0
local function check(condition, what)
    if not condition then
        failures = failures + 1
        print("FAIL: " .. what)
    end
end

local lines, newest_said = {}, {}
local failing = false
local function menu_line(text, newest)
    if failing then error("the line failed") end
    lines[#lines + 1] = text
    newest_said[#newest_said + 1] = newest or "-"
end
local function last() return lines[#lines] end

-- the engine's two functions: they count their calls and hand back what they were given
local registered, unregistered = {}, {}
local function engine_register(menu, name, extra)
    registered[#registered + 1] = name
    return "registered " .. tostring(name), extra
end
local function engine_unregister(menu, name)
    unregistered[#unregistered + 1] = name
    return "unregistered " .. tostring(name)
end

-- a root with an element on it, a menu or not
local function element(menu_name)
    local e = { menuName = menu_name, children = {} }
    function e:getFirstChild() return self.children[1] end
    function e:getNextSibling()
        local siblings = self.parent and self.parent.children or {}
        for i, sibling in ipairs(siblings) do
            if sibling == self then return siblings[i + 1] end
        end
    end
    function e:addElement(child)
        self.children[#self.children + 1] = child
        child.parent = self
    end
    return e
end

-- CoD.Menu as ui/t6/codmenu.lua makes and closes menus: NewFromState and close call the two globals if they're there
local function new_menu(name)
    local menu = element(name)
    if RegisterOpenedMenu then RegisterOpenedMenu(menu, name) end
    return menu
end
local function close_menu(menu)
    if UnregisterOpenedMenu then UnregisterOpenedMenu(menu, menu.menuName) end
end

local chunk = assert(loadfile("source/crash_log.lua"))

-- a fresh UI state: the globals as the engine registers them, the BO3MacFix table, a root
local root
local function new_state(with_engine)
    lines, newest_said, registered, unregistered = {}, {}, {}, {}
    RegisterOpenedMenu = with_engine and engine_register or nil
    UnregisterOpenedMenu = with_engine and engine_unregister or nil
    BO3MacFix = {}
    root = element(nil)
    LUI = { roots = { UIRootFull = root, UIRoot0 = element(nil) } }
    _RegisterOpenedMenu = nil
end

-- ---- the user's way to the globe, and back
new_state(true)
chunk(menu_line, false)
check(last() == "menus: watching this UI's menus", "the first line: " .. tostring(last()))
check(RegisterOpenedMenu ~= engine_register and UnregisterOpenedMenu ~= engine_unregister, "both are wrapped")
local main = new_menu("Main")
check(last() == "menu: Main opened", "Main: " .. tostring(last()))
check(newest_said[#newest_said] == "Main", "the newest is Main")
close_menu(main)
check(last() == "menu: Main closed", "Main closed: " .. tostring(last()))
check(newest_said[#newest_said] == "-", "no menu open after Main closed")
local lobby = new_menu("Lobby")
check(last() == "menu: Lobby opened", "Lobby: " .. tostring(last()))
local change = new_menu("ChangeMap")
check(last() == "menu: ChangeMap opened over Lobby", "ChangeMap: " .. tostring(last()))
local globe = new_menu("ZMGlobeMapSelect")
check(last() == "menu: ZMGlobeMapSelect opened over ChangeMap", "the globe: " .. tostring(last()))
check(newest_said[#newest_said] == "ZMGlobeMapSelect", "the newest is the globe")
close_menu(globe)
check(last() == "menu: ZMGlobeMapSelect closed", "the globe closed: " .. tostring(last()))
check(newest_said[#newest_said] == "ChangeMap", "the newest is ChangeMap again")
globe = new_menu("ZMGlobeMapSelect")
check(last() == "menu: ZMGlobeMapSelect opened over ChangeMap", "the globe again: " .. tostring(last()))
-- a menu below the newest closes: the newest stays
close_menu(change)
check(last() == "menu: ChangeMap closed", "ChangeMap closed under the globe")
check(newest_said[#newest_said] == "ZMGlobeMapSelect", "the globe is still the newest")
close_menu(globe)
check(newest_said[#newest_said] == "Lobby", "then the lobby")
-- two open by one name: closing the older one leaves the newer, by which menu it is, not by name
local x1 = new_menu("X")
local y = new_menu("Y")
local x2 = new_menu("X")
close_menu(x1)
check(newest_said[#newest_said] == "X", "the newer X stays the newest: " .. newest_said[#newest_said])
close_menu(x2)
check(newest_said[#newest_said] == "Y", "then Y: " .. newest_said[#newest_said])
close_menu(y)
-- one closing that was never seen opening
close_menu(element("Unseen"))
check(last() == "menu: Unseen closed", "a menu never seen opening")
check(newest_said[#newest_said] == "Lobby", "the lobby stays the newest")
close_menu(lobby)
-- eight made (the globe twice, X twice), nine closed (one never seen opening)
check(#registered == 8 and #unregistered == 9, "the engine saw every menu: " .. #registered .. " " .. #unregistered)
check(registered[4] == "ZMGlobeMapSelect" and unregistered[2] == "ZMGlobeMapSelect" and unregistered[8] == "Unseen",
      "with their names")

-- ---- every argument and result as before, also when the line fails
local a, b = RegisterOpenedMenu(element("X"), "X", 7)
check(a == "registered X" and b == 7, "results handed back: " .. tostring(a) .. " " .. tostring(b))
failing = true
local count = #registered
local ok, result = pcall(RegisterOpenedMenu, element("Y"), "Y")
check(ok and result == "registered Y" and #registered == count + 1, "a failing line keeps the engine's call")
ok, result = pcall(UnregisterOpenedMenu, element("Y"), "Y")
check(ok and result == "unregistered Y", "and the close's")
failing = false

-- ---- loading again in the same state wraps nothing twice
local wrapped = RegisterOpenedMenu
chunk(menu_line, false)
check(RegisterOpenedMenu == wrapped, "not wrapped twice")
count = #lines
new_menu("Z")
check(#lines == count + 1, "one line for one menu")

-- ---- the mod's wrapper (conditions.lua: _RegisterOpenedMenu = RegisterOpenedMenu, then its own), after ours
local mod_calls = 0
local function mod_wraps()
    _RegisterOpenedMenu = RegisterOpenedMenu
    RegisterOpenedMenu = function(menu, name)
        mod_calls = mod_calls + 1
        return _RegisterOpenedMenu(menu, name)
    end
end
mod_wraps()
count, mod_calls = #lines, 0
local before = #registered
new_menu("AfterMod")
check(#lines == count + 1 and mod_calls == 1 and #registered == before + 1, "the mod's wrapper over ours")
-- and before ours, in a new state
new_state(true)
mod_wraps()
chunk(menu_line, false)
new_menu("BeforeMod")
check(last() == "menu: BeforeMod opened" and mod_calls == 2 and #registered == 1, "ours over the mod's wrapper")

-- ---- menus open already, and debug's list of the open menus
new_state(true)
root:addElement(element("main"))
root:addElement(element(nil))
root:addElement(element("Splash"))
chunk(menu_line, true)
check(last() == "menus: watching this UI's menus, open already: main, Splash [open: main > Splash]",
      "open already: " .. tostring(last()))
new_menu("Main")
check(last() == "menu: Main opened over Splash [open: main > Splash > Main]", "debug's list: " .. tostring(last()))
-- a root that can't be walked: no list, the rest works
new_state(true)
LUI.roots.Broken = {}
chunk(menu_line, false)
check(last() == "menus: watching this UI's menus", "a broken root: " .. tostring(last()))
new_menu("Main")
check(last() == "menu: Main opened", "menus after a broken root")

-- ---- at most 32 menus are kept; the newest stays right
new_state(true)
chunk(menu_line, false)
local many = {}
for i = 1, 40 do many[i] = new_menu("M" .. i) end
check(last() == "menu: M40 opened over M39", "the 40th: " .. tostring(last()))
close_menu(many[40])
check(newest_said[#newest_said] == "M39", "after the 40th closed: " .. newest_said[#newest_said])
for i = 39, 1, -1 do close_menu(many[i]) end
check(newest_said[#newest_said] == "-", "all closed")

-- ---- a menu the game drops without closing it isn't kept alive by the list
new_state(true)
chunk(menu_line, false)
local probe = setmetatable({}, { __mode = "k" })
do
    local dropped = new_menu("Dropped")
    probe[dropped] = true
end
collectgarbage()
collectgarbage()
local alive = 0
for _ in pairs(probe) do alive = alive + 1 end
check(alive == 0, "a dropped menu was kept alive")
check(newest_said[#newest_said] == "Dropped", "its line was written")
-- a close without a menu takes nothing off the list, not even the dropped menu's place
UnregisterOpenedMenu(nil, "Ghost")
check(newest_said[#newest_said] == "Dropped", "a close of nil changed the list: " .. newest_said[#newest_said])
-- one that stays open is still known for its close
local kept_open = new_menu("Kept")
collectgarbage()
close_menu(kept_open)
check(last() == "menu: Kept closed" and newest_said[#newest_said] == "Dropped", "the open one closes: " .. last())

-- ---- a UI without the engine's functions, or without the line: nothing is wrapped
new_state(false)
chunk(menu_line, false)
check(last() == "menus: this UI has no RegisterOpenedMenu, so its menus aren't logged", "no engine: " .. tostring(last()))
check(RegisterOpenedMenu == nil and BO3MacFix.CrashLogMenus == nil, "nothing set")
new_state(true)
chunk(nil, false)
check(RegisterOpenedMenu == engine_register and #lines == 0, "no line function: nothing wrapped")
new_state(true)
BO3MacFix = nil
chunk(menu_line, false)
check(RegisterOpenedMenu == engine_register, "no BO3MacFix table: nothing wrapped")

if failures == 0 then
    print("crash_log.lua: all tests passed")
end
os.exit(failures == 0 and 0 or 1)
