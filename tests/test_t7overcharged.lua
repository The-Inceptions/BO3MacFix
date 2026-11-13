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
-- T7Overcharged's line does; started when T7Overcharged starts, and by every menu made after it (the loading screen's
-- with NewFromState too) for a root without a working timer
local function root(name)
    local r = { name = name, timers = {} }
    function r:addElement(element) table.insert(self.timers, element) end
    return r
end
local pending, enabled, called, clock, playing = {}, 0, {}, 0, false
local function takeCustomKey() return table.remove(pending, 1) end
local made = {}
local function freshUI()
    fresh()
    pending, enabled, called, made, clock, playing = {}, 0, {}, {}, 1000, false
    Engine.milliseconds = function() return clock end
    Engine.IsCinematicPlaying = function() return playing end
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
local function start() chunk(noop, noop, noop, noop, nativePrint, noop, noop, false, takeCustomKey) end
local loadingUp = "customkeymap: the loading screen is up and the keys are taken on UIRoot0, UIRootFull (menu Loading)"
local function stopped(ms)
    return "customkeymap: the loading movie stopped " .. ms .. " ms after the loading screen came up"
end
local function lineWith(text)
    for _, line in ipairs(lines) do
        if line:find(text, 1, true) then return line end
    end
end
local function linesWith(text)
    local n = 0
    for _, line in ipairs(lines) do
        if line:find(text, 1, true) then n = n + 1 end
    end
    return n
end

-- each case runs on its own: one that breaks (an error, not only a failed check) is counted and the next still runs
local function case(name, body)
    local ok, err = pcall(body)
    if not ok then
        failures = failures + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

-- T7Overcharged starts and no menu is made after it (the UI state round 5 saw before its map): the keys are taken
case("started with no menu after it, then the game's order", function()
    freshUI()
    start()
    local full, zero = LUI.roots.UIRootFull, LUI.roots.UIRoot0
    check(timers(full) == 1 and timers(zero) == 1, "a timer on each root when T7Overcharged starts")
    check(full.timers[1].ms == 50 and full.timers[1].disposable == false, "every 50 ms, for good")
    check(count("customkeymap: the keys are taken on UIRoot0, UIRootFull (when T7Overcharged starts)") == 1,
          "said once, with why")
    enabled = 0 -- the stand-in itself calls EnableGlobals as it loads
    table.insert(pending, "t7stest")
    full.timers[1].tick()
    check(enabled == 1 and called[1] == "t7stest", "a key is taken without any menu made")
    -- the game's order in a map's start: the HUD, then the loading screen inside it; no second timer, and the loading
    -- screen says which roots take the keys
    local menu = CoD.Menu.NewFromState("HUD", 0)
    check(menu ~= nil and menu.menu == "HUD" and made[1] == "HUD", "the HUD's menu is still made")
    menu = CoD.Menu.NewFromState("Loading", 0)
    check(menu ~= nil and menu.menu == "Loading" and made[2] == "Loading", "the loading screen's menu is still made")
    check(timers(full) == 1 and timers(zero) == 1, "still one timer on each root")
    check(count(loadingUp) == 1,
          "the loading screen says the keys are taken")
    -- a tick with nothing that fired: nothing called
    enabled, called = 0, {}
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
    check(timers(full) == 1 and timers(zero) == 1 and made[3] == "StartMenu_Main", "no second timer")
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
end)

-- an older caller without the take function: nothing wrapped
case("an older caller", function()
    freshUI()
    local plain = CoD.Menu.NewFromState
    chunk(noop, noop, noop, noop, nativePrint, noop, noop, false)
    check(CoD.Menu.NewFromState == plain, "no take function: menus made as they were")
    check(timers(LUI.roots.UIRootFull) == 0, "no take function: no timer either")
end)

-- the loading screen made before T7Overcharged starts in its Lua state: the keys are taken there all the same
case("the loading screen made before the start", function()
    freshUI()
    CoD.Menu.NewFromState("Loading", 0)
    check(timers(LUI.roots.UIRootFull) == 0 and timers(LUI.roots.UIRoot0) == 0, "no timer before T7Overcharged starts")
    start()
    check(timers(LUI.roots.UIRootFull) == 1 and timers(LUI.roots.UIRoot0) == 1, "its start puts one on each root")
    enabled = 0
    table.insert(pending, "loadingskip1")
    LUI.roots.UIRoot0.timers[1].tick()
    check(called[1] == "loadingskip1" and enabled == 1, "Escape at that loading screen calls the mod's function")
    -- started again in the same Lua state: no second timer, nothing said again
    start()
    check(timers(LUI.roots.UIRootFull) == 1 and timers(LUI.roots.UIRoot0) == 1, "a second start adds no timer")
    check(count("customkeymap: the keys are taken on UIRoot0, UIRootFull (when T7Overcharged starts)") == 1,
          "the second start says nothing")
    CoD.Menu.NewFromState("HUD", 0)
    check(timers(LUI.roots.UIRootFull) == 1, "nor does a menu after it")
end)

-- no root yet when T7Overcharged starts: nothing then, the first menu after it puts the timers on
case("no root at the start", function()
    freshUI()
    local roots = LUI.roots
    LUI.roots = {}
    start()
    check(lineWith("customkeymap: the keys are taken on") == nil, "no root: nothing taken, nothing said")
    LUI.roots = roots
    CoD.Menu.NewFromState("HUD", 0)
    check(timers(roots.UIRootFull) == 1 and timers(roots.UIRoot0) == 1, "the first menu puts them on")
    check(count("customkeymap: the keys are taken on UIRoot0, UIRootFull (menu HUD)") == 1, "and says so")
end)

-- a timer taken away (something emptied its root): the next menu puts one back once it hasn't ticked for over 5 s;
-- one that ticks, or has been quiet for less, is left alone
case("a timer taken away", function()
    freshUI()
    start()
    local zero = LUI.roots.UIRoot0
    clock = clock + 4000
    zero.timers = {}
    CoD.Menu.NewForUIEditor("A")
    check(timers(zero) == 0, "quiet for 4 s: still counted")
    LUI.roots.UIRootFull.timers[1].tick()
    clock = clock + 2000
    CoD.Menu.NewForUIEditor("B")
    check(timers(zero) == 1, "quiet for 6 s: a new timer")
    check(timers(LUI.roots.UIRootFull) == 1, "one that ticked 2 s ago is left alone")
    check(count("customkeymap: the keys are taken on UIRoot0 (menu B)") == 1, "and says which root")
    table.insert(pending, "loadingskip1")
    zero.timers[1].tick()
    check(called[1] == "loadingskip1", "the new one takes the keys")
    LUI.roots.UIRootFull = root("UIRootFull")
    CoD.Menu.NewForUIEditor("C")
    check(timers(LUI.roots.UIRootFull) == 1, "a new root of the same name gets its own")
    -- a root that is gone isn't named by the loading screen
    LUI.roots.UIRoot1 = root("UIRoot1")
    CoD.Menu.NewForUIEditor("D")
    check(timers(LUI.roots.UIRoot1) == 1, "another root, another timer")
    LUI.roots.UIRoot1 = nil
    CoD.Menu.NewFromState("Loading", 0)
    check(count(loadingUp) == 1, "the loading screen names the roots there are")
end)

-- the loading movie: when it stops, counted from the loading screen's menu (AAE's skip stops it)
case("the loading movie", function()
    freshUI()
    start()
    CoD.Menu.NewFromState("HUD", 0)
    CoD.Menu.NewFromState("Loading", 0)
    local tick = LUI.roots.UIRoot0.timers[1].tick
    clock = 1050
    tick()
    check(lineWith("customkeymap: the loading movie stopped") == nil, "nothing before the movie plays")
    playing, clock = true, 2000
    tick()
    check(lineWith("customkeymap: the loading movie stopped") == nil, "nothing while it plays")
    playing, clock = false, 4500
    tick()
    check(count(stopped(3500)) == 1, "the stop, timed")
    clock = 5000
    LUI.roots.UIRootFull.timers[1].tick()
    check(linesWith("customkeymap: the loading movie stopped") == 1, "said once")
    -- a loading screen whose movie doesn't start within 60 s: no longer watched
    CoD.Menu.NewFromState("Loading", 0)
    clock = 66000
    tick()
    playing, clock = true, 67000
    tick()
    playing, clock = false, 68000
    tick()
    check(lineWith("after the loading screen came up") ~= nil and count(stopped(63000)) == 0,
          "a movie long after the loading screen isn't counted")
end)

-- a game without Engine.milliseconds: the keys are taken all the same, nothing timed, no timer put on twice
case("no clock", function()
    freshUI()
    Engine.milliseconds = nil
    start()
    CoD.Menu.NewFromState("Loading", 0)
    check(count(loadingUp) == 1,
          "without a clock the loading screen says so too")
    playing = true
    LUI.roots.UIRoot0.timers[1].tick()
    playing = false
    LUI.roots.UIRoot0.timers[1].tick()
    check(lineWith("customkeymap: the loading movie stopped") == nil, "without a clock nothing is timed")
    CoD.Menu.NewForUIEditor("X")
    check(timers(LUI.roots.UIRoot0) == 1 and timers(LUI.roots.UIRootFull) == 1, "without a clock no second timer")
    table.insert(pending, "loadingskip1")
    LUI.roots.UIRootFull.timers[1].tick()
    check(called[1] == "loadingskip1", "without a clock the keys are taken")
end)

if failures > 0 then
    print(failures .. " t7overcharged.lua test(s) failed")
    os.exit(1)
end
print("t7overcharged.lua tests passed")
