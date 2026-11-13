-- Runs source/t7overcharged.lua (the T7Overcharged stand-in) against a made-up Engine and CoD, with any Lua 5.x:
-- lua tests/test_t7overcharged.lua
-- Checks BO3MACFIX_LUAMISSING's watch for missing Engine and CoD functions: off unless asked, one line per missing
-- name, nothing for functions that exist (the game's or the stand-in's), a second load in the same Lua state, and a
-- table that already has a metatable left alone.
local failures = 0
local function check(condition, what)
    if not condition then
        failures = failures + 1
        print("FAIL: " .. what)
    end
end

local chunk = assert(loadfile("source/t7overcharged.lua"))
local lines = {}
local function nativePrint(text) table.insert(lines, text) end
local function noop() end
local function load(logMissing)
    chunk(noop, noop, noop, noop, nativePrint, noop, noop, logMissing)
end
local function count(text)
    local n = 0
    for _, line in ipairs(lines) do
        if line == "[T7Overcharged] " .. text then n = n + 1 end
    end
    return n
end
local function fresh()
    lines = {}
    Engine = { SendMenuResponse = noop, CreateModel = noop, GetModel = noop }
    CoD = { isZombie = true }
end

-- off by default
fresh()
load(false)
check(count("loaded") == 1, "the stand-in loads")
check(getmetatable(Engine) == nil and getmetatable(CoD) == nil, "no watch without BO3MACFIX_LUAMISSING")
check(Engine.NotHere == nil, "a missing Engine function is still nil")
for _, line in ipairs(lines) do
    check(not line:find("no Engine", 1, true), "nothing logged without BO3MACFIX_LUAMISSING: " .. line)
end

-- on: each missing name once
fresh()
load(true)
check(count("loaded") == 1, "the stand-in loads with the watch")
check(Engine.GetRegulations ~= nil and Engine.PartyHostToggleStart ~= nil, "the stand-in's Engine additions are there")
check(Engine.NotHere == nil and Engine.NotHere == nil, "a missing Engine function reads as nil")
check(count("no Engine.NotHere here") == 1, "a missing Engine function is logged once")
check(Engine.GetModel == noop and count("no Engine.GetModel here") == 0, "a function the game has isn't logged")
check(Engine.PartyHostToggleStart ~= nil and count("no Engine.PartyHostToggleStart here") == 0,
      "a function the stand-in added isn't logged")
check(CoD.SomethingNew == nil and count("no CoD.SomethingNew here") == 1, "a missing CoD field is logged")
CoD.SomethingNew = 5
check(CoD.SomethingNew == 5 and count("no CoD.SomethingNew here") == 1, "a field set later reads normally")
local ok = pcall(function() return Engine.NotCallable() end)
check(not ok, "calling a missing function still fails the way it did")

-- loaded again in the same Lua state: the watch is kept, no complaint
load(true)
check(count("loaded") == 2, "the stand-in loads a second time")
check(count("can't watch Engine for missing functions") == 0, "its own watch from the first load is fine")
check(Engine.NotHere == nil and count("no Engine.NotHere here") == 1, "still once per name")

-- a library with a metatable of its own (the game's Engine has one): what its __index answers passes through
fresh()
local function hidden() end
local engineMeta = { __index = { Hidden = hidden } }
setmetatable(Engine, engineMeta)
local theirs = { __index = function(_, name) if name ~= "Unknown" then return "theirs" end end }
setmetatable(CoD, theirs)
load(true)
check(getmetatable(Engine) == engineMeta and getmetatable(CoD) == theirs, "the libraries keep their own metatables")
check(Engine.Hidden == hidden and count("no Engine.Hidden here") == 0, "a table __index's answer passes through")
check(Engine.Gone == nil and Engine.Gone == nil and count("no Engine.Gone here") == 1,
      "a name the table __index doesn't have is logged once")
check(CoD.Anything == "theirs" and count("no CoD.Anything here") == 0, "a function __index's answer passes through")
check(CoD.Unknown == nil and count("no CoD.Unknown here") == 1, "a name it answers nil for is logged")
check(count("can't watch Engine for missing functions") == 0, "nothing refused")
load(true)
check(Engine.Gone == nil and count("no Engine.Gone here") == 1 and Engine.Hidden == hidden,
      "a second load doesn't wrap the __index twice")

-- customkeymap: timers on the UI's roots take the keys (T7OTakeCustomKey) and call the mod's function, as
-- T7Overcharged's line does; started by the first menu made, the loading screen's (NewFromState) too
local function root(name)
    local r = { name = name, timers = {} }
    function r:addElement(element) table.insert(self.timers, element) end
    return r
end
local pending, enabled, called = {}, 0, {}
local function takeCustomKey() return table.remove(pending, 1) end
local made = {}
local function freshUI()
    fresh()
    pending, enabled, called, made = {}, 0, {}, {}
    LUI = { roots = { UIRootFull = root("UIRootFull"), UIRoot0 = root("UIRoot0") },
            UITimer = { newElementTimer = function(ms, disposable, tick)
                return { ms = ms, disposable = disposable, tick = tick }
            end } }
    CoD.Menu = { NewFromState = function(name) table.insert(made, name) return { menu = name } end,
                 NewForUIEditor = function(name) table.insert(made, name) return { menu = name } end }
    EnableGlobals = function() enabled = enabled + 1 end
    CoD.Mod = { CallCustomFunction = function(name) table.insert(called, name) end }
end
local function timers(r) return #r.timers end

freshUI()
chunk(noop, noop, noop, noop, nativePrint, noop, noop, false, takeCustomKey)
local full, zero = LUI.roots.UIRootFull, LUI.roots.UIRoot0
check(timers(full) == 0 and timers(zero) == 0, "no timer before a menu is made")
local menu = CoD.Menu.NewFromState("Loading", 0)
check(menu ~= nil and menu.menu == "Loading" and made[1] == "Loading", "the loading screen's menu is still made")
check(timers(full) == 1 and timers(zero) == 1, "one timer on each root")
check(full.timers[1].ms == 50 and full.timers[1].disposable == false, "every 50 ms, for good")
check(count("customkeymap: the keys are taken on UIRoot0, UIRootFull (menu Loading)") == 1, "said once, with the menu")
-- a tick with nothing that fired: nothing called
enabled = 0
full.timers[1].tick()
check(enabled == 0 and #called == 0, "nothing to take: nothing called")
-- a key that fired: EnableGlobals, then the mod's function with the mapping's name
table.insert(pending, "loadingskip1")
zero.timers[1].tick()
check(enabled == 1 and called[1] == "loadingskip1" and #called == 1, "the mod's function gets the name")
full.timers[1].tick()
check(#called == 1, "taken once")
-- more menus: no more timers; a root made later gets one
CoD.Menu.NewForUIEditor("StartMenu_Main")
check(timers(full) == 1 and timers(zero) == 1 and made[2] == "StartMenu_Main", "no second timer")
LUI.roots.UIRoot1 = root("UIRoot1")
CoD.Menu.NewForUIEditor("Hud")
check(timers(LUI.roots.UIRoot1) == 1 and timers(full) == 1, "a new root gets its own")
check(count("customkeymap: the keys are taken on UIRoot1 (menu Hud)") == 1, "and says so")
-- the mod's function failing, or not there: the timer goes on, the error is logged
CoD.Mod.CallCustomFunction = function() error("broken") end
table.insert(pending, "loadingskip2")
full.timers[1].tick()
local logged = false
for _, line in ipairs(lines) do
    if line:find("[T7Overcharged] customkeymap:", 1, true) and line:find("broken", 1, true) then logged = true end
end
check(logged, "an error in the mod's function is logged")
CoD.Mod = nil
table.insert(pending, "loadingskip1")
full.timers[1].tick()
check(#pending == 0 and enabled == 3, "without CoD.Mod it's taken and nothing called")
-- loaded again in the same state: the makers aren't wrapped twice
local before = CoD.Menu.NewFromState
chunk(noop, noop, noop, noop, nativePrint, noop, noop, false, takeCustomKey)
check(CoD.Menu.NewFromState == before, "a second load leaves the wrapper as it is")
-- an older caller without the take function: nothing wrapped
freshUI()
local plain = CoD.Menu.NewFromState
chunk(noop, noop, noop, noop, nativePrint, noop, noop, false)
check(CoD.Menu.NewFromState == plain, "no take function: menus made as they were")

if failures > 0 then
    print(failures .. " t7overcharged.lua test(s) failed")
    os.exit(1)
end
print("t7overcharged.lua tests passed")
