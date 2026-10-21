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

if failures > 0 then
    print(failures .. " t7overcharged.lua test(s) failed")
    os.exit(1)
end
print("t7overcharged.lua tests passed")
