-- Runs source/checkpoint.lua's AutoSave against a made-up Engine, LUI and C side, with any Lua 5.x:
-- lua tests/test_autosave.lua
-- Checks when a new round saves (not the round a match starts at, once it has stayed, once per round, only solo, only
-- with AutoSave on, not while a resumed game gets its weapons back), the Delete saved game row, and that quitting keeps
-- a deeper save by the HUD's round.
local failures = 0
local function check(condition, what)
    if not condition then
        failures = failures + 1
        print("FAIL: " .. what)
    end
end

-- ---- a made-up game
local models, logs, execs = {}, {}, {}
local map, in_game, lobby_clients, splitscreen = "core_frontend", false, 1, false
local function model(path) return { path = path } end
Engine = {
    GetModelForController = function(c) return model("controller" .. c) end,
    GetGlobalModel = function() return model("global") end,
    GetModel = function(parent, path) return model(parent.path .. "." .. path) end,
    GetModelValue = function(m) return models[m.path] end,
    GetClientNum = function() return 0 end,
    GetScoreboardColumnHeader = function(c, column) return ({ [0] = "MPUI_KILLS", "MPUI_SCORE" })[column] end,
    GetScoreboardColumnForClient = function(client, column) return ({ [0] = 40, 3210 })[column] end,
    Localize = function(text) return (string.gsub(string.lower(text), "^mpui_", "")) end,
    GetCurrentMap = function() return map end,
    IsInGame = function() return in_game end,
    DvarString = function() return "" end,
    Exec = function(_, command) table.insert(execs, command) end,
    IsSplitscreen = function() return splitscreen end,
    GetLobbyClientCount = function(lobby)
        if lobby ~= Enum.LobbyType.LOBBY_TYPE_GAME then return 9 end
        return lobby_clients
    end,
}
Enum = { LobbyType = { LOBBY_TYPE_GAME = 1 } }
CoD = { GetGameRulePostFix = function() return "_Default" end }
local tick
LUI = { roots = { UIRootFull = { addElement = function() end } },
    UITimer = { newElementTimer = function(ms, once, fn) tick = fn return {} end } }

-- ---- a made-up C side
local store, state, weapons, weapon_reads, posts, saves, deleted, cleared = {}, 0, nil, 0, {}, {}, 0, 0
local autosave_on, clock_now = true, 0
local function c_side()
    local save = function(lines)
        local given = {}
        for key, value in string.gmatch(lines, "(%a+)=([^\n]*)") do given[key] = value end
        store = { Round = given.Round, Points = given.Points, Map = given.Map, Weapons = given.Weapons }
        table.insert(saves, given)
        return true
    end
    local read = function()
        local lines = {}
        for key, value in pairs(store) do table.insert(lines, key .. "=" .. value) end
        return table.concat(lines, "\n")
    end
    local apply = function() state = 1 return "" end
    local clear = function()
        if state == 0 then return nil end
        state, cleared = 0, cleared + 1
        return "modvar tfoption_starting_round 7\n"
    end
    local resume = function(mark)
        if mark == 1 and state == 1 then state = 2 elseif mark == 2 and state == 2 then state = 3 end
        return state
    end
    local read_weapons = function() weapon_reads = weapon_reads + 1 return weapons end
    local weapon_commands = function(saved) return "give " .. saved .. "\n" end
    local allow_cheats = function() end
    local notice = function() end
    local autosave = function(lines, note)
        if lines == nil then return autosave_on end
        local given = {}
        for key, value in string.gmatch(lines, "(%a+)=([^\n]*)") do given[key] = value end
        table.insert(posts, { lines = given, note = note })
        return true
    end
    local delete = function()
        if state ~= 0 then return false end
        store, deleted = {}, deleted + 1
        return true
    end
    return save, read, apply, clear, resume, read_weapons, weapon_commands, allow_cheats, notice, autosave, delete
end

BO3MacFix = { Log = function(text) table.insert(logs, text) end, NoticeClock = function() return clock_now end }
local chunk = assert(loadfile("source/checkpoint.lua"))
local save, read, apply, clear, resume, read_weapons, weapon_commands, allow_cheats, notice, autosave, delete = c_side()
chunk(save, read, apply, clear, resume, read_weapons, weapon_commands, allow_cheats, notice, nil, nil, nil, autosave,
    delete, nil)
check(tick ~= nil, "a timer is started")
local function polls(n) for _ = 1, n do tick() end end
local function round(n) models["controller0.gameScore.roundsPlayed"] = n + 1 end -- one more than the HUD's round

local function enter_match(name, first_round)
    map, in_game = name, true
    models = {}
    round(first_round)
    weapons = "ray_gun_upgraded_zm:0,pistol_burst_zm:0,knife_zm:4"
end

local function leave_match()
    map, in_game, models = "core_frontend", false, {}
    polls(1)
end

-- ---- a match that starts at round 7 (AAE's Starting Round)
enter_match("zm_theater", 7)
polls(30)
check(#posts == 0, "the round a match starts at isn't saved: " .. #posts)
round(8)
polls(3)
check(#posts == 0, "not before the new round has stayed")
polls(1)
check(#posts == 1, "the new round saved once it has stayed: " .. #posts)
local post = posts[1] or { lines = {} }
check(post.lines.Round == "8" and post.lines.Points == "3210" and post.lines.Map == "zm_theater",
    "round, points and map: " .. tostring(post.lines.Round) .. ", " .. tostring(post.lines.Points))
check(post.lines.Weapons == "ray_gun_upgraded,pistol_burst", "and the guns: " .. tostring(post.lines.Weapons))
check(type(post.note) == "string" and string.find(post.note, "round 8", 1, true) ~= nil and
    string.find(post.note, "1 in the game's lobby", 1, true) ~= nil, "with what it read, for the log: " .. tostring(post.note))
check(#saves == 0, "by the writer, not SaveCheckpoint")
polls(40)
check(#posts == 1, "once per round")
-- two rounds in quick succession: the newest, once
round(9)
polls(2)
round(10)
polls(3)
check(#posts == 1, "the newest round waits to stay too")
polls(1)
check(#posts == 2 and posts[2].lines.Round == "10", "then it's saved: " .. tostring(posts[2] and posts[2].lines.Round))
polls(10)
check(#posts == 2, "and not again")

-- the map started over: its start round again, then new rounds
round(7)
polls(10)
round(8)
polls(10)
check(#posts == 2, "a restarted map's first seconds only set its round")
polls(20)
round(9)
polls(4)
check(#posts == 3 and posts[3].lines.Round == "9", "then its next round saves")
leave_match()

-- a match whose HUD shows round 1 before AAE's Starting Round takes it to 7
enter_match("zm_theater", 1)
polls(4)
round(7)
polls(30)
check(#posts == 3, "the jump to the starting round isn't a new round")
leave_match()

-- ---- only solo, only zombies, only with AutoSave on
enter_match("zm_theater", 3)
lobby_clients = 2
polls(25)
round(4)
polls(10)
check(#posts == 3, "not with two players in the game's lobby")
local told = 0
for _, line in ipairs(logs) do
    if string.find(line, "AutoSave is for solo games, this one has 2 in the game's lobby", 1, true) then told = told + 1 end
end
check(told == 1, "says so in the log once: " .. told)
round(5)
polls(10)
lobby_clients = 0 -- a game started with +map has no lobby
round(6)
polls(4)
check(#posts == 4 and posts[4].lines.Round == "6", "a game without a lobby is solo")
splitscreen = true
round(7)
polls(10)
check(#posts == 4, "not in split screen")
splitscreen = false
autosave_on = false
local reads = weapon_reads
round(8)
polls(10)
check(#posts == 4, "not with AutoSave off")
check(weapon_reads == reads, "which doesn't even read the game")
autosave_on = true
round(9)
polls(4)
check(#posts == 5 and posts[5].lines.Round == "9", "on again, the next round saves")
leave_match()
lobby_clients = 1
enter_match("mp_nuketown_x", 3)
polls(25)
round(4)
polls(10)
check(#posts == 5, "not outside zombies")
leave_match()

-- ---- a resumed game: not while it gets its weapons back
store = { Round = "20", Points = "50000", Map = "zm_theater", Weapons = "ray_gun_upgraded" }
state = 1
enter_match("zm_theater", 20)
weapons = nil -- the starting weapons aren't there yet
polls(30)
check(state == 2, "the resume is in use")
round(21)
polls(10)
check(#posts == 5, "nothing while the resumed game waits for its weapons")
weapons = "pistol_m1911_zm:0,knife_zm:4"
polls(6)
check(state == 3, "the weapons given back")
check(execs[#execs] == "give ray_gun_upgraded", "by the resume: " .. tostring(execs[#execs]))
polls(2)
check(#posts == 5, "not right after the give commands, before the weapons have arrived")
polls(8)
check(#posts == 6 and posts[6].lines.Round == "21", "then the round the game is at is saved: " ..
    tostring(posts[6] and posts[6].lines.Round))
leave_match()
check(state == 0, "the menus undid the resume")

-- ---- the Delete saved game row
local message
local rows = BO3MacFix.CheckpointRows(0, function(text) message = text end)
local delete_row = rows[3]
check(#rows == 3 and delete_row ~= nil and delete_row.label == "Delete saved game", "a third row")
check(delete_row.kind == "action" and delete_row.category == "Saved game", "an action with the other two")
store = {}
delete_row.run()
check(string.find(message, "no saved game", 1, true) ~= nil, "nothing to delete: " .. tostring(message))
store = { Round = "12", Points = "23450", Map = "zm_theater" }
clock_now = 100
delete_row.run()
check(deleted == 0, "the first press only asks")
check(string.find(message, "Press again to delete Round 12, 23,450 points, zm_theater", 1, true) ~= nil,
    "what it would delete: " .. tostring(message))
check(delete_row.text() == "Press again to delete", "the row says so: " .. tostring(delete_row.text()))
clock_now = 106
delete_row.run()
check(deleted == 0, "a second press after 5 seconds asks again")
clock_now = 108
delete_row.run()
check(deleted == 1 and store.Round == nil, "the second press deletes it")
check(message == "Deleted the saved game.", "and says so: " .. tostring(message))
check(delete_row.text() == "", "the row is back to normal")
check(rows[1].text() == "None saved", "the save row shows it: " .. tostring(rows[1].text()))
-- with a resume set up, the player's own options come back first
store = { Round = "12", Points = "23450", Map = "zm_theater" }
state, execs = 1, {}
delete_row.run()
delete_row.run()
check(cleared == 2 and state == 0 and deleted == 2, "the resume undone, then deleted")
check(execs[1] == "modvar tfoption_starting_round 7", "the player's starting round set again: " .. tostring(execs[1]))
-- the C side refusing it
store = { Round = "12", Points = "23450", Map = "zm_theater" }
local real_clear = clear
local refuse = function() return nil end
rows = nil
BO3MacFix.ClearCheckpoint = refuse
state = 1
local delete_rows = BO3MacFix.CheckpointRows(0, function(text) message = text end)
delete_rows[3].run()
delete_rows[3].run()
check(string.find(message, "Couldn't delete", 1, true) ~= nil and deleted == 2, "a refusal is shown: " .. tostring(message))
BO3MacFix.ClearCheckpoint, state = real_clear, 0

-- ---- quitting keeps a deeper save, by the HUD's round
enter_match("zm_theater", 14)
store = { Round = "15", Points = "1000", Map = "zm_theater" }
local count = #saves
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == count and store.Round == "15", "round 14 doesn't replace a round 15 save")
round(15)
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == count + 1 and saves[#saves].Round == "15", "round 15 does")
leave_match()

if failures == 0 then
    print("test_autosave.lua: all passed")
else
    os.exit(1)
end
