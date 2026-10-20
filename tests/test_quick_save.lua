-- Runs source/checkpoint.lua against a made-up Engine, LUI and C side, with any Lua 5.x: lua tests/test_quick_save.lua
-- Checks the quick save (the QuickSave key, or L3 + R3 held in the pause menu): it saves through the settings page's
-- Save game path with the same checks (a solo zombies match, a resumed game not short of its saved round), says how it
-- went in a notice, and asks again before replacing a game saved at a higher round; and the saved game's details on
-- the settings page's rows (map, round, points, when it was saved).
local failures = 0
local function check(condition, what)
    if not condition then
        failures = failures + 1
        print("FAIL: " .. what)
    end
end

-- ---- a made-up game
local models, logs = {}, {}
local map, in_game, lobby_clients = "core_frontend", false, nil
local function model(path) return { path = path } end
Engine = {
    GetModelForController = function(c) return model("controller" .. c) end,
    GetGlobalModel = function() return model("global") end,
    GetModel = function(parent, path) return model(parent.path .. "." .. path) end,
    GetModelValue = function(m) return models[m.path] end,
    GetClientNum = function() return 0 end,
    GetScoreboardColumnHeader = function(c, column) return ({ [0] = "MPUI_KILLS", "MPUI_SCORE" })[column] end,
    GetScoreboardColumnForClient = function(client, column) return ({ [0] = 40, 2340 })[column] end,
    Localize = function(text) return text end,
    GetCurrentMap = function() return map end,
    IsInGame = function() return in_game end,
    DvarString = function() return "" end,
    Exec = function() end,
    GetLobbyClientCount = function() return lobby_clients end,
}
Enum = { LobbyType = { LOBBY_TYPE_GAME = 2 } }
CoD = { GetGameRulePostFix = function() return "_Default" end }
local tick
local root = { addElement = function() end }
LUI = { roots = { UIRootFull = root }, UITimer = { newElementTimer = function(ms, once, fn) tick = fn return {} end } }

-- ---- a made-up checkpoint.c, quick_save.m and notices
local store, state, saves, notices, requests, clock_now = {}, 0, {}, {}, {}, 100
local SAVED = { "Map", "Round", "Points", "Perks", "Weapons", "Equipment", "Held", "Character", "Stats" }
local function save_fn(lines)
    local given = {}
    for key, value in string.gmatch(lines, "(%a+)=([^\n]*)") do given[key] = value end
    for _, key in ipairs(SAVED) do store[key] = given[key] end
    store.Saved = "1000"
    table.insert(saves, given)
    return true
end
local function read_fn()
    local lines = {}
    for key, value in pairs(store) do table.insert(lines, key .. "=" .. value) end
    return table.concat(lines, "\n")
end
local function resume_fn() return state end
local function notice_fn(text) table.insert(notices, text) end
-- CheckpointTakeQuickSave: one request a call, as the C side's slot
local function take_fn()
    local request = table.remove(requests, 1)
    if request == nil then return nil end
    if request == "pad" then return "pad" end
    return "key", "F5"
end
local when_asked = {}
local function when_fn(saved)
    table.insert(when_asked, saved)
    return saved == "1000" and "3 h ago" or ""
end
BO3MacFix = { Log = function(text) table.insert(logs, text) end, NoticeClock = function() return clock_now end }
local chunk = assert(loadfile("source/checkpoint.lua"))
chunk(save_fn, read_fn, function() return nil end, function() return nil end, resume_fn, function() return nil end,
    function() return "" end, function() end, notice_fn, nil, nil, nil, nil, nil, nil, nil, nil, nil, nil, nil, nil,
    take_fn, when_fn)
check(tick ~= nil, "a timer is started")
check(BO3MacFix.CheckpointTakeQuickSave == take_fn and BO3MacFix.CheckpointSavedWhen == when_fn,
    "the new C functions are set up")

local function enter_match(name, hud_round)
    map, in_game = name, true
    models = { ["controller0.gameScore.roundsPlayed"] = hud_round + 1 }
end
local function press(source)
    table.insert(requests, source or "key")
    notices = {}
    tick()
    return notices[#notices]
end

-- ---- the key
-- in the menus: not saved, and the notice says why
local last = press()
check(#saves == 0 and last == "Not saved: start a solo zombies game first", "in the menus: " .. tostring(last))
-- nothing asked for: nothing happens
notices = {}
tick()
check(#notices == 0 and #saves == 0, "no request, no notice")

-- a solo zombies match with nothing saved: saved at once, as the settings page's row does
enter_match("zm_theater", 7)
last = press()
check(#saves == 1 and saves[1].Map == "zm_theater" and saves[1].Round == "7" and saves[1].Points == "2340",
    "saved: " .. tostring(saves[1] and saves[1].Round))
check(last == "Game saved: round 7, 2,340 points", "the notice: " .. tostring(last))
check(string.find(table.concat(logs, "\n"), "checkpoint: saving (quick save): round 7", 1, true) ~= nil,
    "the log says it was the quick save")
-- the same lines as the settings page's Save game
local message
local rows = BO3MacFix.CheckpointRows(0, function(text) message = text end)
rows[1].run()
check(#saves == 2 and message == "Saved Round 7, 2,340 points, Kino der Toten.", "the page's row: " .. tostring(message))
local same = true
for key, value in pairs(saves[1]) do if saves[2][key] ~= value then same = false end end
for key in pairs(saves[2]) do if saves[1][key] == nil then same = false end end
check(same, "the quick save writes what the page's row writes")

-- the same round, or a deeper one, saves at once
enter_match("zm_theater", 9)
last = press()
check(#saves == 3 and store.Round == "9" and last == "Game saved: round 9, 2,340 points", "deeper: " .. tostring(last))

-- not a zombies game, not solo
enter_match("mp_nuketown_x", 3)
last = press()
check(#saves == 3 and last == "Not saved: quick save is for zombies games", "multiplayer: " .. tostring(last))
rows[1].run()
check(#saves == 3 and message == "Save game is for zombies games.", "the page's row too: " .. tostring(message))
enter_match("zm_theater", 9)
lobby_clients = 2
last = press()
check(#saves == 3 and last == "Not saved: quick save is for solo games, and this one has 2 in the game's lobby",
    "co-op: " .. tostring(last))
rows[1].run()
check(#saves == 3 and message == "Save game is for solo games, and this one has 2 in the game's lobby.",
    "the page's row too: " .. tostring(message))
lobby_clients = 1
last = press()
check(#saves == 4, "one client is solo: " .. tostring(last))

-- a resumed game short of its saved round keeps the saved game (the page row's guard)
store = { Map = "zm_theater", Round = "20", Points = "9000", AppliedRound = "20", Saved = "1000" }
state = 2
enter_match("zm_theater", 1)
last = press()
check(#saves == 4 and store.Round == "20" and last == "Not saved: this resumed game isn't at its saved round 20 yet",
    "a resume short of its round: " .. tostring(last))
-- at its round it saves, without asking (this game resumed the deeper one)
enter_match("zm_theater", 20)
last = press()
check(#saves == 5 and store.Round == "20" and last == "Game saved: round 20, 2,340 points", "at its round: " .. tostring(last))
state = 3
store.Round = "25"
enter_match("zm_theater", 22)
last = press()
check(#saves == 6 and store.Round == "22", "a resumed game replaces its own deeper save: " .. tostring(last))
state = 0

-- a deeper game this one didn't resume: the first press asks, a second within 5 seconds replaces it
store = { Map = "zm_der_riese", Round = "38", Points = "105660", Saved = "1000" }
enter_match("zm_theater", 3)
clock_now = 200
last = press()
check(#saves == 6 and store.Round == "38" and last == "Saved game is round 38 on Der Riese: press F5 again to replace it",
    "asks first: " .. tostring(last))
clock_now = 206
last = press()
check(#saves == 6 and store.Round == "38", "after 5 seconds it asks again: " .. tostring(last))
clock_now = 208
last = press()
check(#saves == 7 and store.Round == "3" and last == "Game saved: round 3, 2,340 points", "the second press saves: " ..
    tostring(last))
-- and the next press, the save no longer deeper, saves at once
last = press()
check(#saves == 8, "then at once")

-- ---- L3 + R3: only in the pause menu
store = {}
local menu_parent = nil
local pause_menu = { getParent = function() return menu_parent end }
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", pause_menu)
notices = {}
last = press("pad")
check(#saves == 8 and #notices == 0, "playing (the pause menu closed): nothing, not even a notice")
menu_parent = { getParent = function() return root end }
last = press("pad")
check(#saves == 9 and last == "Game saved: round 3, 2,340 points", "in the pause menu: saved")
store = { Map = "zm_der_riese", Round = "38", Points = "105660", Saved = "1000" }
last = press("pad")
check(#saves == 9 and last == "Saved game is round 38 on Der Riese: hold L3 + R3 again to replace it",
    "the pad's ask: " .. tostring(last))
menu_parent = nil
last = press("pad")
check(#saves == 9, "the pause menu closed: nothing")
-- another menu made doesn't count as the pause menu
BO3MacFix.CheckpointMenuCreated("SomeOtherMenu", { getParent = function() return root end })
check(#saves == 9, "still nothing")

-- ---- the settings page's rows: map, round, points and when it was saved
local resume_row, delete_row = rows[2], rows[3]
store = { Map = "zm_der_riese", Round = "38", Points = "105660", Saved = "1000" }
state = 0
check(rows[1].text() == "Round 38, 105,660 points, Der Riese", "the save row: " .. tostring(rows[1].text()))
check(resume_row.text() == "Off (round 38, saved 3 h ago)", "the resume row: " .. tostring(resume_row.text()))
check(delete_row.text() == "Der Riese, 3 h ago", "the delete row: " .. tostring(delete_row.text()))
check(when_asked[#when_asked] == "1000", "the time asked for as text: " .. tostring(when_asked[#when_asked]))
-- an older save without a time, or a stock map's name
store = { Map = "zm_castle", Round = "4", Points = "500" }
check(resume_row.text() == "Off (round 4)" and delete_row.text() == "Der Eisendrache",
    "without a time: " .. resume_row.text() .. " / " .. delete_row.text())
store = { Round = "4", Points = "500", Saved = "1000" }
check(delete_row.text() == "3 h ago", "without a map: " .. delete_row.text())
store = {}
check(resume_row.text() == "Off" and delete_row.text() == "" and rows[1].text() == "None saved", "nothing saved")
-- a resume set up says which round, as before
store = { Map = "zm_theater", Round = "12", Points = "23450", AppliedRound = "12", Saved = "1000" }
state = 1
check(resume_row.text() == "Next game: round 12", "set up: " .. resume_row.text())

if failures == 0 then
    print("test_quick_save.lua: all passed")
else
    os.exit(1)
end
