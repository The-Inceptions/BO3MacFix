-- ResumeKeepsBank in source/checkpoint.lua, against a made-up Engine, LUI and C side, with any Lua 5.x:
-- lua tests/test_resume_bank.lua
-- A resumed game starts with the mod's 500 points; the rest of the saved points (checkpoint.ini's PointsLater) are
-- handed over a moment after the first points are earned, once, and count in any save made before that.
local failures = 0
local function check(condition, what)
    if not condition then
        failures = failures + 1
        print("FAIL: " .. what)
    end
end

-- ---- a made-up game
local models, execs, logs, notices, saves = {}, {}, {}, {}, {}
local map, in_game, score = "core_frontend", false, 500
local function model(path) return { path = path } end
-- ---- a made-up checkpoint.c and zm_inventory.c
local store, state = {}, 0
local waiting, given, banned, runs_commands = 0, false, 0, true
local watch_over = 1 -- the mod's level.is_earned_points: 1 set, 0 not yet, -1 can't be read
local map_starts, lobby_clients = 0, 1 -- zm_inventory.c's count of map starts; the players in the game's lobby
local root = { addElement = function() end }
Enum = { LobbyType = { LOBBY_TYPE_GAME = 1 } }
Engine = {
    GetModelForController = function(c) return model("controller" .. c) end,
    GetGlobalModel = function() return model("global") end,
    GetModel = function(parent, path) return model(parent.path .. "." .. path) end,
    GetModelValue = function(m) return models[m.path] end,
    GetClientNum = function() return 0 end,
    GetScoreboardColumnHeader = function(c, column) return ({ [0] = "MPUI_KILLS", "MPUI_SCORE", "MPUI_DOWNS" })[column] end,
    GetScoreboardColumnForClient = function(client, column) return ({ [0] = 12, score, 0 })[column] end,
    Localize = function(text) return "\21 " .. (string.gsub(string.lower(text), "^mpui_", "")) .. " \20" end,
    GetCurrentMap = function() return map end,
    IsInGame = function() return in_game end,
    DvarString = function() return "" end,
    GetLobbyClientCount = function() return lobby_clients end,
    -- the game runs the give command on its server: the points come through its own function
    Exec = function(_, command)
        table.insert(execs, command)
        if command == "give bo3macfix_points" and runs_commands and waiting > 0 and not given then
            score, waiting, given = score + waiting, 0, true
        end
    end,
}
CoD = { GetGameRulePostFix = function() return "_Default" end }
local tick
LUI = { roots = { UIRootFull = root }, UITimer = { newElementTimer = function(ms, once, fn) tick = fn return {} end } }
local later_calls = {}
local fake = {
    Log = function(text) table.insert(logs, text) end,
    ReadCheckpoint = function()
        local lines = {}
        for key, value in pairs(store) do table.insert(lines, key .. "=" .. value) end
        return table.concat(lines, "\n")
    end,
    SaveCheckpoint = function(lines)
        local got = {}
        for key, value in string.gmatch(lines, "(%a+)=([^\n]*)") do got[key] = value end
        table.insert(saves, got)
        return true
    end,
    CheckpointResume = function(mark)
        if mark == 1 and state == 1 then state = 2 elseif mark == 2 and state == 2 then state = 3 end
        return state
    end,
    CheckpointWeapons = function() return "pistol_standard_zm:0,knife_zm:4" end,
    CheckpointAllowCheats = function(commands) return 0 end,
    CheckpointNotice = function(text) table.insert(notices, text) end,
    ClearCheckpoint = function() state = 0 return "" end,
    -- 0 what the resume still owes, 1 set it up and the command, 2 the mod's ban, 3 what became of the command
    CheckpointPointsLater = function(what)
        table.insert(later_calls, what or 0)
        if what == 2 then return banned end
        if what == 3 then return given and 2 or (waiting > 0 and runs_commands) and 1 or 0 end
        if what == 4 then return watch_over end
        if what == 5 then return map_starts end
        local rest = state >= 1 and tonumber(store.PointsLater) or 0
        if what == 1 then
            -- once for each start of the map, whatever is asked
            if given or rest <= 0 then return "" end
            waiting = rest
            if not runs_commands then waiting = 0 end -- (the 30 s pass at once)
            return "give bo3macfix_points"
        end
        return rest
    end,
}
BO3MacFix = { Log = fake.Log }
local chunk = assert(loadfile("source/checkpoint.lua"))
-- settings_menu.c's order: 9 functions, 3 test switches, AutoSave's two and its switch, the loadout's four, a switch,
-- the gun in hand, the quick save's, the saved time's, then ResumeKeepsBank's
chunk(fake.SaveCheckpoint, fake.ReadCheckpoint, nil, fake.ClearCheckpoint, fake.CheckpointResume, fake.CheckpointWeapons,
    nil, fake.CheckpointAllowCheats, fake.CheckpointNotice, nil, nil, nil, nil, nil, nil, nil, nil, nil, nil, nil, nil,
    nil, nil, fake.CheckpointPointsLater)
check(tick ~= nil and BO3MacFix.CheckpointPointsLater == fake.CheckpointPointsLater, "set up")
local function polls(n) for _ = 1, n do tick() end end
local function count(list, text)
    local n = 0
    for _, item in ipairs(list) do if string.find(tostring(item), text, 1, true) then n = n + 1 end end
    return n
end
local function enter_match(played)
    map, in_game = "zm_theater", true
    models = { ["controller0.gameScore.roundsPlayed"] = played }
end
local function leave_match()
    map, in_game, models = "core_frontend", false, {}
    polls(2)
end
local function resume(points, later)
    store = { Map = "zm_theater", Round = "23", Points = tostring(points), PointsLater = later and tostring(later) or nil }
    state, score, waiting, given, banned, runs_commands, watch_over = 1, 500, 0, false, 0, true, 1
    map_starts, lobby_clients = map_starts + 1, 1
    execs, logs, notices, saves, later_calls = {}, {}, {}, {}, {}
    enter_match(24)
end
local function save_now()
    local rows = BO3MacFix.CheckpointRows(0, function() end)
    rows[1].run()
    return saves[#saves] or {}
end

-- ---- the saved points above 500 come after the first points earned, once
resume(3880, 3380)
polls(2)
check(count(notices, "Resumed round 23 with 3,880 points") == 1 and count(notices, "3,380 of them come with the first") == 1,
    "the resume says what comes later: " .. tostring(notices[1]))
polls(120) -- a minute without earning anything: nothing is handed over
check(count(execs, "bo3macfix_points") == 0, "nothing before the first points earned")
check(save_now().Points == "3880", "a save before that has all the points: " .. tostring(save_now().Points))
score = 560 -- a hit
polls(1)
check(count(logs, "the first points were earned (500 -> 560)") == 1, "the first points are seen")
polls(2)
check(count(execs, "bo3macfix_points") == 0, "not at once")
polls(1)
check(count(execs, "give bo3macfix_points") == 1 and score == 560 + 3380, "handed over 1.5 s later: " .. tostring(score))
polls(1)
check(count(notices, "Your other 3,380 points are back") == 1, "and said")
check(save_now().Points == "3940", "a save after it has what the game shows: " .. tostring(save_now().Points))
score = 4000
polls(200)
check(count(execs, "bo3macfix_points") == 1, "only once in a match")
check(count(logs, "10 s after its other points came: open") == 1, "the mod's bank is reported once")
check(count(notices, "switched its bank off") == 0, "no notice while the bank is open")
leave_match()

-- ---- the mod bans anyway: the player is told
resume(3880, 3380)
polls(12)
score, banned = 510, 1
polls(40)
check(score == 510 + 3380 and count(logs, "switched off by the mod") == 1 and
      count(notices, "All-around Enhancement has switched its bank off for this game") == 1, "a ban is told")
leave_match()

-- ---- spending first is not earning; the first rise after it is
resume(1500, 1000)
polls(12)
score = 0 -- bought something
polls(20)
check(count(execs, "bo3macfix_points") == 0 and count(logs, "the first points were earned") == 0, "spending isn't earning")
score = 10
polls(5)
check(count(logs, "the first points were earned (0 -> 10)") == 1 and score == 1010, "the first rise after it is")
leave_match()

-- ---- the points a game starts with can come late (an intro): their arrival is not the first points earned
resume(3880, 3380)
score = 0
polls(30)
score = 500
polls(30)
check(count(execs, "bo3macfix_points") == 0 and count(logs, "the first points were earned") == 0,
    "the start's own 500 arriving isn't points earned")
score = 630
polls(5)
check(score == 630 + 3380, "the points earned after it are")
leave_match()

-- ---- a game that never shows 500 (the player's own options differ): after 30 s what it has is the start
resume(3880, 3380)
score = 750
polls(70)
check(count(execs, "bo3macfix_points") == 0, "nothing while the points stay")
score = 760
polls(5)
check(score == 760 + 3380, "the first rise after the 30 s is the first points earned")
leave_match()

-- ---- the mod's watch of the scores is still on although the points rose: nothing until it is over
resume(3880, 3380)
watch_over = 0
polls(12)
score = 560
polls(60)
check(count(execs, "bo3macfix_points") == 0 and count(logs, "the mod's watch of the scores is still on") == 1,
    "nothing while the mod still watches")
watch_over = 1
polls(2)
check(score == 560 + 3380, "handed over once its watch is over")
leave_match()
-- a mod whose bank isn't running never sets the flag: after a minute the points come anyway
resume(3880, 3380)
watch_over = 0
polls(12)
score = 560
polls(118)
check(count(execs, "bo3macfix_points") == 0, "not within the minute")
polls(6)
check(score == 560 + 3380, "after a minute without the flag")
leave_match()
-- the flag can't be read: the points' rise decides alone
resume(3880, 3380)
watch_over = -1
polls(12)
score = 560
polls(5)
check(score == 560 + 3380, "handed over by the rise alone when the flag can't be read")
leave_match()
-- a start that isn't 500 (the saved points weren't a multiple of 10): 491 here
resume(4321, 3830)
score = 491
polls(12)
check(count(logs, "the first points were earned") == 0, "the start's 491 is the start")
score = 511
polls(5)
check(count(logs, "the first points were earned (491 -> 511)") == 1 and score == 511 + 3830, "491 and the rest: " .. tostring(score))
leave_match()

-- ---- the game doesn't run the command (paused, or the player down): sent again 5 s later, five times at most
resume(3880, 3380)
runs_commands = false
polls(12)
score = 560
polls(4)
check(count(execs, "bo3macfix_points") == 1, "sent")
polls(9)
check(count(execs, "bo3macfix_points") == 1, "not again before 5 s")
polls(4)
check(count(execs, "bo3macfix_points") == 2, "again after 5 s")
polls(200)
check(count(execs, "bo3macfix_points") == 5 and count(notices, "Saved points not given back") == 1,
    "five times, then given up and said: " .. count(execs, "bo3macfix_points"))
check(score == 560 and save_now().Points == "3940", "and a save still has them: " .. tostring(save_now().Points))
leave_match()
-- the second try works
resume(3880, 3380)
runs_commands = false
polls(12)
score = 560
polls(6)
runs_commands = true
polls(14)
check(count(execs, "bo3macfix_points") == 2 and score == 3940 and count(notices, "Your other 3,380 points are back") == 1,
    "the second command gives them: " .. tostring(score))
leave_match()

-- ---- the map starts over: its points start again, and it is owed them again
resume(3880, 3380)
polls(12)
score = 560
polls(6)
check(score == 3940, "given")
models["controller0.gameScore.roundsPlayed"] = 26
polls(4)
models["controller0.gameScore.roundsPlayed"] = 24 -- a restart: back to the round the resume starts at
score, map_starts, given = 500, map_starts + 1, false
polls(14)
check(save_now().Points == "3880", "owed again after a restart: " .. tostring(save_now().Points))
score = 510
polls(6)
check(score == 510 + 3380 and count(execs, "bo3macfix_points") == 2, "and given again in the new game")
leave_match()

-- a restart in the round the resume starts at: the round doesn't go back, the map's start is counted
resume(3880, 3380)
polls(12)
score = 560
polls(6)
check(score == 3940, "given")
score, map_starts, given = 500, map_starts + 1, false
polls(14)
check(save_now().Points == "3880", "owed again after a restart in the same round: " .. tostring(save_now().Points))
score = 520
polls(6)
check(score == 520 + 3380 and count(execs, "bo3macfix_points") == 2, "and given again")
leave_match()
-- a round that reads lower without the map having started over: the same game, which has had its points
resume(3880, 3380)
polls(12)
score = 560
polls(6)
models["controller0.gameScore.roundsPlayed"] = 23
polls(3)
models["controller0.gameScore.roundsPlayed"] = 24
score = 3950
polls(100)
score = 4000
polls(100)
check(count(execs, "bo3macfix_points") == 1 and score == 4000, "no second hand-over in the same start of the map")
check(save_now().Points == "4000", "and its saves are the game's points: " .. tostring(save_now().Points))
leave_match()

-- ---- a save in the half second between the server's hand-over and the next poll
resume(3880, 3380)
polls(12)
score = 560
polls(3)
check(count(execs, "bo3macfix_points") == 0, "about to be sent")
polls(1) -- sent, and the game ran it at once: the score has the points, the poll hasn't seen it yet
check(score == 3940 and save_now().Points == "3940", "a save right after the hand-over counts them once: " ..
    tostring(save_now().Points))
leave_match()
-- the other way round: the server has handed them over, the scoreboard doesn't show it yet
resume(3880, 3380)
polls(12)
score = 560
polls(4)
score = 560 -- (the made-up game added them at once: take them out of what the scoreboard shows)
check(save_now().Points == "3940", "a save before the scoreboard shows them has them too: " .. tostring(save_now().Points))
score = 3940
leave_match()

-- ---- the pause menu open: nothing is sent into the pause, and a long pause doesn't use the sends up
resume(3880, 3380)
polls(12)
local pause_menu = { getParent = function() return root end }
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", pause_menu)
score = 560 -- (the last hit before the pause)
polls(600) -- five minutes paused
check(count(execs, "bo3macfix_points") == 0 and count(notices, "not given back") == 0, "nothing sent while paused")
pause_menu.getParent = function() return nil end
polls(3)
check(count(execs, "bo3macfix_points") == 1 and score == 3940, "sent once the pause is over: " .. tostring(score))
leave_match()

-- ---- a game with other players: nothing is handed over
resume(3880, 3380)
lobby_clients = 2
polls(12)
score = 560
polls(200)
check(count(execs, "bo3macfix_points") == 0 and count(notices, "not given back") == 0, "not in a game with others")
leave_match()

-- ---- nothing owed (the setting off, or 500 points or fewer): no command, and the bank's state is logged once
resume(3880, nil)
score = 3880
polls(30)
score = 3900
polls(100)
check(count(execs, "bo3macfix_points") == 0 and count(later_calls, "1") == 0, "nothing handed over without PointsLater")
check(count(logs, "40 s in: open") == 1, "the bank's state 40 s into a resumed game")
check(count(notices, "Resumed round 23 with 3,880 points") == 1 and count(notices, "come with the first") == 0,
    "the old notice")
check(save_now().Points == "3900", "a save has the game's points")
leave_match()

-- ---- a game that isn't a resume: nothing at all
state, store, later_calls, execs, score = 0, { Map = "zm_theater", Round = "5", Points = "9000", PointsLater = "8500" }, {}, {}, 9000
enter_match(6)
polls(200)
check(#later_calls == 0 and count(execs, "bo3macfix_points") == 0, "no resume, nothing asked or given")
check(save_now().Points == "9000", "and saves are the game's points")
leave_match()

if failures == 0 then
    print("test_resume_bank.lua: all passed")
else
    print("test_resume_bank.lua: " .. failures .. " failed")
    os.exit(1)
end
