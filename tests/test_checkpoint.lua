-- Runs source/checkpoint.lua against a made-up Engine, LUI and C side, with any Lua 5.x: lua tests/test_checkpoint.lua
-- Checks what a save reads from the HUD's models, the scoreboard and the weapons, the settings rows, a resumed match
-- (marked in use, a summary shown, the saved weapons given once the starting ones have stayed), putting the player's
-- options back in the menus, and saving when quitting.
local failures = 0
local function check(condition, what)
    if not condition then
        failures = failures + 1
        print("FAIL: " .. what)
    end
end

-- ---- a made-up game
local models, dvars, execs, logs = {}, {}, {}, {}
local map, in_game = "core_frontend", false
local function model(path) return { path = path } end
Engine = {
    GetModelForController = function(c) return model("controller" .. c) end,
    GetGlobalModel = function() return model("global") end,
    GetModel = function(parent, path) return model(parent.path .. "." .. path) end,
    GetModelValue = function(m) return models[m.path] end,
    GetClientNum = function() return 0 end,
    GetScoreboardColumnHeader = function(c, column) return ({ [0] = "MPUI_KILLS", "MPUI_SCORE", "MPUI_DOWNS", "MPUI_REVIVES", "MPUI_HEADSHOTS" })[column] end,
    GetScoreboardColumnForClient = function(client, column) return ({ [0] = 812, 23450, 1, 3, 240 })[column] end,
    Localize = function(text) return "\21 " .. (string.gsub(string.lower(text), "^mpui_", "")) .. " \20" end,
    GetCurrentMap = function() return map end,
    IsInGame = function() return in_game end,
    DvarString = function(_, name) return dvars[name] or "" end,
    Exec = function(_, command) table.insert(execs, command) end,
}
CoD = { GetGameRulePostFix = function() return "_Default" end }
local tick
local root = { addElement = function() end }
LUI = { roots = { UIRootFull = root }, UITimer = { newElementTimer = function(ms, once, fn) tick = fn return {} end } }

-- ---- a made-up checkpoint.c and zm_inventory.c
local store, state, weapons, notices, saves, plans, allowed, cleared_with = {}, 0, nil, {}, {}, {}, nil, nil
local SAVED = { "Map", "Round", "Points", "Perks", "Weapons", "Equipment", "Held", "Character", "Stats" }
BO3MacFix = {
    Log = function(text) table.insert(logs, text) end,
    ReadCheckpoint = function()
        local lines = {}
        for key, value in pairs(store) do table.insert(lines, key .. "=" .. value) end
        return table.concat(lines, "\n")
    end,
    SaveCheckpoint = function(lines)
        local given = {}
        for key, value in string.gmatch(lines, "(%a+)=([^\n]*)") do given[key] = value end
        for _, key in ipairs(SAVED) do store[key] = given[key] end
        table.insert(saves, given)
        return true
    end,
    ApplyCheckpoint = function(preset)
        store.AppliedPreset, store.AppliedRound, store.AppliedPoints = preset, store.Round, store.Points
        state = 1
        return "modvar tfoption_starting_round " .. store.Round .. "\nmodvar tfoption_starting_points " .. store.Points .. "\n"
    end,
    ClearCheckpoint = function(current)
        if state == 0 then return nil end
        cleared_with, state = current, 0
        store.AppliedPreset, store.AppliedRound, store.AppliedPoints = nil, nil, nil
        return "modvar tfoption_starting_round 7\nmodvar tfoption_starting_points 500\n"
    end,
    CheckpointResume = function(mark)
        if mark == 1 and state == 1 then state = 2 elseif mark == 2 and state == 2 then state = 3 end
        return state
    end,
    CheckpointWeapons = function() return weapons end,
    CheckpointWeaponCommands = function(saved, held)
        table.insert(plans, { saved = saved, held = held })
        return "take pistol_m1911\ngive ray_gun_upgraded\n"
    end,
    CheckpointAllowCheats = function(count) allowed = count end,
    CheckpointNotice = function(text) table.insert(notices, text) end,
}

-- settings_menu.c passes checkpoint.lua the C functions, then the test switches
local fake = BO3MacFix
BO3MacFix = { Log = fake.Log }
local function c_side()
    return fake.SaveCheckpoint, fake.ReadCheckpoint, fake.ApplyCheckpoint, fake.ClearCheckpoint, fake.CheckpointResume,
        fake.CheckpointWeapons, fake.CheckpointWeaponCommands, fake.CheckpointAllowCheats, fake.CheckpointNotice
end
local chunk = assert(loadfile("source/checkpoint.lua"))
local function load(save_after, give_test, report_after)
    local f = { c_side() }
    chunk(f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], save_after, give_test, report_after)
end
load(nil, nil, nil)
check(BO3MacFix.ReadCheckpoint == fake.ReadCheckpoint, "the C functions are set up")
check(BO3MacFix.CheckpointRows ~= nil and BO3MacFix.CheckpointMenuCreated ~= nil, "the entry points are there")
check(tick ~= nil, "a timer is started")
local function polls(n) for _ = 1, n do tick() end end

local function enter_match(name)
    map, in_game = name, true
    models = {
        ["controller0.gameScore.roundsPlayed"] = 13, -- one more than the HUD's round
        ["controller0.PlayerList.client0.playerScore"] = 3,
        ["controller0.hudItems.perks.juggernaut"] = 1,
        ["controller0.hudItems.perks.quick_revive"] = 2,
        ["controller0.hudItems.perks.doubletap2"] = 1,
        ["controller0.hudItems.perks.marathon"] = 0,
        ["controller0.currentWeapon.weaponName"] = "WEAPON_RAYGUN",
        ["controller0.currentWeapon.viewmodelWeaponName"] = "ray_gun_upgraded",
        ["controller0.zmInventory.player_character_identity"] = 2,
        ["controller0.PlayerList.client0.zombiePlayerIcon"] = "uie_t7_zm_hud_score_char3",
    }
    weapons = "ray_gun_upgraded_zm:0,pistol_burst_zm:0,frag_grenade_zm:1,knife_zm:4"
end

local function leave_match()
    map, in_game, models = "core_frontend", false, {}
end

-- ---- the settings rows
local message
local rows = BO3MacFix.CheckpointRows(0, function(text) message = text end)
local save_row, resume_row = rows[1], rows[2]
check(save_row.kind == "action" and resume_row.kind == "action", "two action rows")
check(save_row.text() == "None saved", "nothing saved yet: " .. tostring(save_row.text()))
save_row.run()
check(string.find(message, "during a zombies game", 1, true) ~= nil, "saving in the menus explains: " .. tostring(message))
resume_row.run()
check(string.find(message, "Save a game first", 1, true) ~= nil, "resuming without a save explains: " .. tostring(message))

-- saving in a match: everything the HUD, the scoreboard and the weapons say
enter_match("zm_theater")
save_row.run()
local saved = saves[#saves]
check(saved ~= nil, "saved")
saved = saved or {}
check(saved.Map == "zm_theater" and saved.Round == "12" and saved.Points == "23450", "map, round and points")
check(saved.Perks == "juggernaut,quick_revive,doubletap2", "perks owned or paused, by id: " .. tostring(saved.Perks))
check(saved.Weapons == "ray_gun_upgraded,pistol_burst", "primary weapons: " .. tostring(saved.Weapons))
check(saved.Equipment == "frag_grenade,knife", "the rest: " .. tostring(saved.Equipment))
check(saved.Held == "weapon_raygun (Pack-a-Punched)", "the weapon in hand: " .. tostring(saved.Held))
check(saved.Character == "Richtofen", "the character on Kino: " .. tostring(saved.Character))
check(saved.Stats == "kills 812, downs 1, revives 3, headshots 240", "the scoreboard but the score: " .. tostring(saved.Stats))
check(string.find(message, "Saved Round 12, 23,450 points, zm_theater", 1, true) ~= nil, "the message: " .. tostring(message))
check(string.find(save_row.description(), "ray gun (Pack-a-Punched)", 1, true) ~= nil, "the description lists the guns: " ..
    tostring(save_row.description()))
check(string.find(save_row.description(), "3 perks", 1, true) ~= nil, "and the perks")
-- on Shadows of Evil the identity isn't one of Dempsey's crew, so the icon's name stands in
map = "zm_zod"
save_row.run()
check(saves[#saves].Character == "uie_t7_zm_hud_score_char3", "Shadows' character: " .. tostring(saves[#saves].Character))
map = "zm_theater"
save_row.run()
-- without the weapons (no game hosted here) the save has none, so a resume leaves the starting ones
weapons = nil
save_row.run()
check(saves[#saves].Weapons == nil, "no weapons line without them")
weapons = "ray_gun_upgraded_zm:0,pistol_burst_zm:0,frag_grenade_zm:1,knife_zm:4"
save_row.run()
-- resuming is for the menus
resume_row.run()
check(string.find(message, "main menu", 1, true) ~= nil, "resuming in a match explains: " .. tostring(message))
check(state == 0, "nothing set up in a match")

-- ---- resuming
leave_match()
polls(2)
execs = {}
resume_row.run()
check(state == 1, "set up")
check(execs[1] == "modvar tfoption_starting_round 12" and execs[2] == "modvar tfoption_starting_points 23450", "options set in the game")
check(resume_row.text() == "Next game: round 12", "the row says so: " .. tostring(resume_row.text()))
polls(4)
check(state == 1, "still set up in the menus")

-- the resumed match: nothing before its HUD is up
enter_match("zm_theater")
models["controller0.gameScore.roundsPlayed"] = nil
polls(3)
check(state == 1 and #notices == 0, "waits for the HUD")
models["controller0.gameScore.roundsPlayed"] = 13
-- the starting weapons, not yet stayed long enough
weapons = "pistol_m1911_zm:0,frag_grenade_zm:1,knife_zm:4"
execs, plans = {}, {}
polls(1)
check(state == 2, "marked in use")
check(notices[1] == "Resumed round 12 with 23,450 points, every perk", "the summary: " .. tostring(notices[1]))
check(notices[2] ~= nil and string.find(notices[2], "character Richtofen", 1, true) ~= nil, "what isn't restored: " ..
    tostring(notices[2]))
polls(8)
check(#plans == 0, "no weapons before they've stayed: " .. #plans)
polls(4)
check(#plans == 1, "the weapons planned once")
check(plans[1] and plans[1].saved == "ray_gun_upgraded,pistol_burst" and plans[1].held == "pistol_m1911",
    "saved and held primaries: " .. tostring(plans[1] and plans[1].held))
check(allowed == 2, "two commands let through: " .. tostring(allowed))
check(execs[1] == "take pistol_m1911" and execs[2] == "give ray_gun_upgraded", "run: " .. tostring(execs[1]))
check(state == 3, "weapons given")
weapons = "ray_gun_upgraded_zm:0,pistol_burst_zm:0,frag_grenade_zm:1,knife_zm:4"
polls(10)
check(#plans == 1, "never again")
check(string.find(notices[#notices], "Guns back: ray gun (Pack-a-Punched), pistol burst", 1, true) ~= nil,
    "says what it has now: " .. tostring(notices[#notices]))

-- back in the menus: the player's own options again
dvars = { tfoption_starting_round = "12", tfoption_starting_points = "23450", tfoption_perkaholic = "1" }
leave_match()
execs = {}
polls(1)
check(state == 0, "cleared")
check(cleared_with ~= nil and string.find(cleared_with, "tfoption_starting_round=12\n", 1, true) ~= nil and
    string.find(cleared_with, "tfoption_spawn_with_quick_res=", 1, true) ~= nil, "with the game's values")
check(execs[1] == "modvar tfoption_starting_round 7", "the player's own values set again")

-- a resume on another map: round and points, but not that map's weapons
resume_row.run()
enter_match("zm_factory")
weapons = "pistol_standard_zm:0"
plans = {}
polls(20)
check(state == 3 and #plans == 0, "no weapons on another map")
leave_match()
polls(1)

-- ---- quitting saves
enter_match("zm_theater")
models["controller0.gameScore.roundsPlayed"] = 16
local count = #saves
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == count + 1 and saves[#saves].Round == "15", "quitting saves")
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", {})
check(#saves == count + 1, "other menus don't")
-- but not over a deeper game this one didn't resume
models["controller0.gameScore.roundsPlayed"] = 5
BO3MacFix.CheckpointMenuCreated("EndGamePopup", {})
check(#saves == count + 1 and store.Round == "15", "a round 15 save stays")
-- the test switches: give, then save and report so many seconds in
leave_match()
polls(1)
BO3MacFix.CheckpointMenuCreated = nil
load("2", "ray_gun_upgraded", "3")
enter_match("zm_theater")
execs, logs = {}, {}
polls(30)
check(execs[1] == "take pistol_m1911" or execs[1] == "give ray_gun_upgraded", "the test give runs: " .. tostring(execs[1]))
local logged = table.concat(logs, "\n")
check(string.find(logged, "checkpoint test: saved true", 1, true) ~= nil, "the test save runs")
check(string.find(logged, "checkpoint test: this match has round 12", 1, true) ~= nil, "the test report runs")

if failures == 0 then
    print("test_checkpoint.lua: all passed")
else
    os.exit(1)
end
