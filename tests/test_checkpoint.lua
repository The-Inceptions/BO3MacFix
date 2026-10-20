-- Runs source/checkpoint.lua against a made-up Engine, LUI and C side, with any Lua 5.x: lua tests/test_checkpoint.lua
-- Checks what a save reads from the HUD's models, the scoreboard and the weapons, the settings rows, a resumed match
-- (marked in use, a summary shown, the saved weapons given once the starting ones have stayed), putting the player's
-- options back in the menus, and saving when quitting; then the same with the full loadout (zm_inventory.c's guns
-- with attachments, options and ammo types, and perks), restored through its give command.
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
local held_commands, allowed_left = false, 0
local clear_fails = false -- checkpoint.ini or AAE's mutations file can't be read or written
local SAVED = { "Map", "Round", "Points", "Perks", "Weapons", "Equipment", "Held", "Character", "Stats", "Guns",
    "GunInHand", "PerkList" }
local loadout, restore_plans, restore_result, test_loadouts = nil, 0, nil, {}
local switch_answers, switch_calls = {}, 0 -- CheckpointRestoreSwitch's answers, one a call, then nil
BO3MacFix = {
    CheckpointRestoreSwitch = function()
        switch_calls = switch_calls + 1
        return table.remove(switch_answers, 1)
    end,
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
        if state == 0 or clear_fails then return nil end
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
    -- the commands to let through; with no argument, how many the game hasn't run yet: all while it holds them (paused)
    CheckpointAllowCheats = function(commands)
        if commands == nil then return held_commands and allowed_left or 0 end
        allowed, allowed_left = commands, 0
        for _ in string.gmatch(commands, "[^\n]+") do allowed_left = allowed_left + 1 end
        return allowed_left
    end,
    CheckpointNotice = function(text) table.insert(notices, text) end,
    CheckpointLoadout = function() return loadout end,
    CheckpointRestoreLoadout = function()
        if store.Guns == nil and store.PerkList == nil then return nil end
        restore_plans = restore_plans + 1
        return "give bo3macfix_restore"
    end,
    CheckpointRestoreResult = function()
        local result = restore_result
        restore_result = nil
        return result
    end,
    CheckpointTestLoadout = function(guns, perks)
        table.insert(test_loadouts, { guns = guns, perks = perks })
        return "give bo3macfix_restore"
    end,
}

-- settings_menu.c passes checkpoint.lua the C functions, then the test switches
local fake = BO3MacFix
BO3MacFix = { Log = fake.Log }
local function c_side()
    return fake.SaveCheckpoint, fake.ReadCheckpoint, fake.ApplyCheckpoint, fake.ClearCheckpoint, fake.CheckpointResume,
        fake.CheckpointWeapons, fake.CheckpointWeaponCommands, fake.CheckpointAllowCheats, fake.CheckpointNotice,
        fake.CheckpointLoadout, fake.CheckpointRestoreLoadout, fake.CheckpointRestoreResult, fake.CheckpointTestLoadout,
        fake.CheckpointRestoreSwitch
end
local chunk = assert(loadfile("source/checkpoint.lua"))
local function load(save_after, give_test, report_after, perks_test)
    local f = { c_side() }
    chunk(f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], save_after, give_test, report_after, nil, nil, nil,
        f[10], f[11], f[12], f[13], perks_test, f[14])
end
load(nil, nil, nil, nil)
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
check(string.find(message, "Saved Round 12, 23,450 points, Kino der Toten", 1, true) ~= nil, "the message: " .. tostring(message))
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
check(allowed == "take pistol_m1911\ngive ray_gun_upgraded\n", "the two commands let through: " .. tostring(allowed))
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

-- a resumed game saved before its guns are back (quitting at once) keeps the saved guns, not the starting pistol
check(store.Map == "zm_theater" and store.Weapons == "ray_gun_upgraded,pistol_burst", "the saved guns before")
resume_row.run()
enter_match("zm_theater")
weapons = "pistol_m1911_zm:0,frag_grenade_zm:1,knife_zm:4"
polls(1)
check(state == 2, "a resumed game, its guns not back yet")
local before = #saves
-- the game's own loadout is the starting pistol's then; this older save (no Guns, no PerkList) must not take it
loadout = "Guns=pistol_m1911_zm:0:0:\nGunInHand=pistol_m1911_zm\nPerkList="
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
loadout = nil
check(#saves == before + 1 and saves[#saves].Weapons == "ray_gun_upgraded,pistol_burst" and
    saves[#saves].Equipment == "frag_grenade,knife", "quitting keeps the saved guns: " .. tostring(saves[#saves].Weapons))
check(saves[#saves].Guns == nil and saves[#saves].PerkList == nil and store.Guns == nil and store.PerkList == nil,
    "an older save stays one, without the starting pistol's Guns line: " .. tostring(saves[#saves].Guns))
-- likewise when no weapons can be read (a save without them would drop the saved ones)
weapons = nil
save_row.run()
check(#saves == before + 2 and saves[#saves].Weapons == "ray_gun_upgraded,pistol_burst", "and without the weapons: " ..
    tostring(saves[#saves].Weapons))
leave_match()
polls(1)
check(state == 0, "cleared after that game")

-- the pause menu open as the resume sends the give and take commands: the game holds them (live on Kino: run 15 s
-- later, after the pause), so the guns are told once it has run them, and a save meanwhile keeps the saved guns
resume_row.run()
enter_match("zm_theater")
weapons = "pistol_m1911_zm:0,frag_grenade_zm:1,knife_zm:4"
execs, notices, logs, held_commands = {}, {}, {}, true
polls(13)
check(state == 3 and execs[1] == "take pistol_m1911" and allowed == "take pistol_m1911\ngive ray_gun_upgraded\n",
    "sent: " .. tostring(execs[1]))
polls(30)
check(not string.find(table.concat(notices, "\n"), "Guns back", 1, true), "no Guns back while the game holds them: " ..
    tostring(notices[#notices]))
local held_saves = #saves
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == held_saves + 1 and saves[#saves].Weapons == "ray_gun_upgraded,pistol_burst",
    "a save meanwhile keeps the saved guns: " .. tostring(saves[#saves].Weapons))
-- unpaused: the game runs them
held_commands = false
weapons = "ray_gun_upgraded_zm:0,pistol_burst_zm:0,frag_grenade_zm:1,knife_zm:4"
polls(7)
check(string.find(notices[#notices] or "", "Guns back: ray gun (Pack-a-Punched), pistol burst", 1, true) ~= nil,
    "told once they're back: " .. tostring(notices[#notices]))
check(string.find(table.concat(logs, "\n"), "s after they were sent (paused?)", 1, true) ~= nil, "and logged")
leave_match()
polls(1)

-- a resume that can't be undone (a file that can't be written) says so and stays on, rather than saying it's off
resume_row.run()
clear_fails, logs = true, {}
resume_row.run()
check(state == 1 and string.find(message, "Couldn't turn the resume off", 1, true) ~= nil,
    "turning it off fails visibly: " .. tostring(message))
-- back in the menus after a resumed game: tried again on every poll, and the log says so once
enter_match("zm_theater")
polls(1)
leave_match()
polls(3)
local stuck = 0
for _, line in ipairs(logs) do
    if string.find(line, "couldn't undo the resume", 1, true) then stuck = stuck + 1 end
end
check(state == 2 and stuck == 1, "still on, logged once: " .. stuck)
clear_fails = false
polls(1)
check(state == 0, "undone once it can be")

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
-- one round behind it too: roundsPlayed 15 is the HUD's round 14
models["controller0.gameScore.roundsPlayed"] = 15
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == count + 1 and store.Round == "15", "a round 15 save stays when quitting at round 14: " ..
    tostring(store.Round))
-- the same round replaces it
models["controller0.gameScore.roundsPlayed"] = 16
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == count + 2 and store.Round == "15", "quitting at the same round saves")
-- the quit and end-game popups are system overlays (CoD.OverlayUtility.CreateOverlay), not menus of their own name:
-- the End Game popup of AAE's start menu didn't save live while only menus were watched
local overlays_made = {}
CoD.OverlayUtility = { CreateOverlay = function(controller, menu, name, extra)
    table.insert(overlays_made, name .. ":" .. tostring(extra))
    return { overlay = name }
end }
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", {})
local before_overlays = #saves
local overlay = CoD.OverlayUtility.CreateOverlay(0, {}, "EndGamePopup", "x")
check(#saves == before_overlays + 1 and overlay ~= nil and overlay.overlay == "EndGamePopup" and
    overlays_made[1] == "EndGamePopup:x", "an End Game overlay saves, and is still made with its arguments")
CoD.OverlayUtility.CreateOverlay(0, {}, "SomeOtherPopup")
check(#saves == before_overlays + 1 and #overlays_made == 2, "other overlays don't save")
-- something that wraps ours, then ours again around that: still one save for one overlay
local inner = CoD.OverlayUtility.CreateOverlay
CoD.OverlayUtility.CreateOverlay = function(...) return inner(...) end
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", {})
CoD.OverlayUtility.CreateOverlay(0, {}, "QuitPCGamePopup")
check(#saves == before_overlays + 2 and #overlays_made == 3, "one save through two wrappers: " ..
    (#saves - before_overlays))
-- an overlay that can't be made still fails (after saving), and the next one saves again
CoD.OverlayUtility = { CreateOverlay = function() error("no such overlay") end }
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", {})
check(not pcall(CoD.OverlayUtility.CreateOverlay, 0, {}, "QuitGamePopup") and #saves == before_overlays + 3,
    "a failing overlay still fails")
CoD.OverlayUtility = { CreateOverlay = function(controller, menu, name) return name end }
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", {})
check(CoD.OverlayUtility.CreateOverlay(0, {}, "QuitGamePopup") == "QuitGamePopup" and #saves == before_overlays + 4,
    "and the next one saves again")
CoD.OverlayUtility = nil
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
BO3MacFix.CheckpointMenuCreated("StartMenu_Main", {})
check(string.find(table.concat(logs, "\n"), "checkpoint test: menu StartMenu_Main", 1, true) ~= nil,
    "the report switch logs the menus a match makes")

-- ---- the full loadout: each gun with its attachments, options and ammo type, and the perks
leave_match()
polls(1)
BO3MacFix.CheckpointMenuCreated = nil
load(nil, nil, nil, nil)
local LOADOUT_GUNS = "ar_standard_upgraded_zm+reflex+extclip:822a:0:zm_aat_dead_wire,ray_gun_upgraded_zm:2a:0:"
loadout = "Guns=" .. LOADOUT_GUNS .. "\nGunInHand=ray_gun_upgraded_zm\nPerkList=specialty_armorvest,specialty_doubletap2"
rows = BO3MacFix.CheckpointRows(0, function(text) message = text end)
save_row, resume_row = rows[1], rows[2]
enter_match("zm_theater")
save_row.run()
saved = saves[#saves]
check(saved.Guns == LOADOUT_GUNS and saved.GunInHand == "ray_gun_upgraded_zm" and
    saved.PerkList == "specialty_armorvest,specialty_doubletap2", "the loadout is saved: " .. tostring(saved.Guns))
check(saved.Weapons == "ray_gun_upgraded,pistol_burst", "with the names too")
local description = save_row.description()
check(string.find(description, "ar standard (Pack-a-Punched, 2 attachments, Dead Wire)", 1, true) ~= nil and
    string.find(description, "ray gun (Pack-a-Punched)", 1, true) ~= nil, "the guns described: " .. description)
check(string.find(resume_row.description, "camo", 1, true) ~= nil, "the resume row says camo comes back")

-- resumed: the summary doesn't promise every perk, and the loadout comes back through its give command
leave_match()
polls(1)
resume_row.run()
check(state == 1, "set up with the loadout")
enter_match("zm_theater")
weapons = "pistol_m1911_zm:0,frag_grenade_zm:1,knife_zm:4"
execs, plans, notices, restore_plans = {}, {}, {}, 0
polls(1)
check(state == 2 and notices[1] == "Resumed round 12 with 23,450 points", "the summary: " .. tostring(notices[1]))
polls(11)
check(restore_plans == 1 and #plans == 0, "the loadout set up, not the names' commands: " .. restore_plans)
check(execs[#execs] == "give bo3macfix_restore", "its give command run: " .. tostring(execs[#execs]))
check(state == 3, "weapons given")
local said = #notices
polls(2)
check(#notices == said, "nothing said before the game answers")
restore_result = "took 1, gave 2 guns (0 held already), 1 ammo types, 2 perks (0 had already), 0 failed"
weapons = "ar_standard_upgraded_zm+reflex+extclip:0,ray_gun_upgraded_zm:0,frag_grenade_zm:1,knife_zm:4"
-- after the restore: watching ("") while the intro has the player, then the switch back, then nothing more (nil)
switch_answers, switch_calls = { "", "", "give bo3macfix_switch", "" }, 0
polls(1)
check(string.find(notices[#notices - 1] or "", "Guns back: ar standard (Pack-a-Punched, 2 attachments, Dead Wire), " ..
    "ray gun (Pack-a-Punched)", 1, true) ~= nil, "the guns back: " .. tostring(notices[#notices - 1]))
check(notices[#notices] == "Perks back: 2", "the perks back: " .. tostring(notices[#notices]))
logged = table.concat(logs, "\n")
check(string.find(logged, "loadout restored: took 1, gave 2 guns", 1, true) ~= nil, "logged")
polls(10)
check(restore_plans == 1, "only once")
local switched = 0
for _, line in ipairs(execs) do if line == "give bo3macfix_switch" then switched = switched + 1 end end
check(switched == 1 and switch_calls == 5, "the switch back's command run once, then no more polls: " .. switched ..
    ", " .. switch_calls)
check(string.find(table.concat(logs, "\n"), "switching back to the saved gun in hand", 1, true) ~= nil, "logged")
-- something that couldn't be restored is said
leave_match()
polls(1)
resume_row.run()
enter_match("zm_theater")
weapons = "pistol_m1911_zm:0"
notices = {}
polls(12)
restore_result = "took 1, gave 1 guns (0 held already), 0 ammo types, 2 perks (0 had already), 1 failed: no weapon x_zm"
polls(1)
check(notices[#notices] == "Not restored: no weapon x_zm", "what failed: " .. tostring(notices[#notices]))
-- a restore that didn't run (the player wasn't alive) says so, not "Guns back", and saves keep the saved loadout
leave_match()
polls(1)
resume_row.run()
enter_match("zm_theater")
weapons = "pistol_m1911_zm:0"
notices = {}
polls(12)
restore_result = "not for the local player, or it isn't alive"
polls(1)
check(notices[#notices] == "Saved loadout not restored: not for the local player, or it isn't alive" and
    not string.find(table.concat(notices, "\n"), "Guns back", 1, true), "not restored: " .. tostring(notices[#notices]))
local live = loadout
loadout = "Guns=pistol_standard_zm:0:0:\nGunInHand=pistol_standard_zm\nPerkList="
before = #saves
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
loadout = live
check(#saves == before + 1 and saves[#saves].Guns == LOADOUT_GUNS, "a save after keeps the saved guns: " ..
    tostring(saves[#saves].Guns))
-- a game that never runs the command (paused): sent again twice, then logged
leave_match()
polls(1)
resume_row.run()
enter_match("zm_theater")
logs, restore_plans, execs = {}, 0, {}
polls(80)
check(restore_plans == 2 and execs[#execs] == "give bo3macfix_restore", "sent again once it's late: " .. restore_plans)
polls(140)
logged = table.concat(logs, "\n")
check(restore_plans == 3, "three times in all: " .. restore_plans)
check(string.find(logged, "wasn't restored", 1, true) ~= nil, "no answer is logged")
-- one that ran too late for the plan (the game says nothing was set up) is sent again at once
leave_match()
polls(1)
resume_row.run()
enter_match("zm_theater")
restore_plans, notices = 0, {}
polls(13)
check(restore_plans == 1, "the loadout's command sent")
restore_result = "nothing set up (or set up too long ago)"
polls(1)
check(restore_plans == 2, "sent again: " .. restore_plans)
restore_result = "took 1, gave 1 guns (0 held already), 0 ammo types, 0 perks (0 had already), 0 failed"
polls(1)
check(string.find(notices[#notices - 1] or notices[#notices] or "", "Guns back", 1, true) ~= nil, "then restored")
-- an answer that comes in on the poll the wait runs out is taken, not sent again over
leave_match()
polls(1)
resume_row.run()
enter_match("zm_theater")
restore_plans, notices = 0, {}
for _ = 1, 30 do if restore_plans == 0 then polls(1) end end
check(restore_plans == 1, "the loadout's command sent once")
polls(62)
check(restore_plans == 1, "still waiting: " .. restore_plans)
restore_result = "took 1, gave 1 guns (0 held already), 0 ammo types, 0 perks (0 had already), 0 failed"
polls(1)
check(restore_plans == 1 and string.find(table.concat(notices, "\n"), "Guns back", 1, true) ~= nil,
    "the late answer taken: " .. restore_plans)

-- quitting a resumed game before the loadout is back keeps the saved one
leave_match()
polls(1)
resume_row.run()
enter_match("zm_theater")
loadout = "Guns=pistol_standard_zm:0:0:\nGunInHand=pistol_standard_zm\nPerkList="
models["controller0.hudItems.perks.juggernaut"] = 0
models["controller0.hudItems.perks.doubletap2"] = 0
polls(1)
check(state == 2, "resumed, not restored")
before = #saves
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == before + 1 and saves[#saves].Guns == LOADOUT_GUNS and
    saves[#saves].PerkList == "specialty_armorvest,specialty_doubletap2" and
    saves[#saves].Perks == "juggernaut,quick_revive,doubletap2", "the saved loadout kept: " .. tostring(saves[#saves].Guns))
leave_match()
polls(1)

-- a resumed game whose HUD isn't at its saved round yet (Ascension's intro, the HUD's round not moved yet, the
-- restore pending): a quit, the page's row or the test switch don't write a lower round over the checkpoint
store.Round, store.Points = "38", "12340"
resume_row.run()
check(state == 1 and store.AppliedRound == "38", "a round 38 resume set up")
enter_match("zm_cosmodrome")
models["controller0.gameScore.roundsPlayed"] = nil -- the intro: no HUD round yet
before = #saves
BO3MacFix.CheckpointMenuCreated("EndGamePopup", {})
check(#saves == before and store.Round == "38", "no save during the intro: " .. tostring(store.Round))
models["controller0.gameScore.roundsPlayed"] = 2 -- the HUD up at round 1 for a moment, the resume in use
polls(1)
check(state == 2, "in use")
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == before and store.Round == "38", "no round 1 over round 38: " .. tostring(store.Round))
message = nil
save_row.run()
check(#saves == before and message ~= nil and string.find(message, "saved round 38", 1, true) ~= nil,
    "the page's row says why: " .. tostring(message))
-- at its round: saved as before (the saved loadout kept while its restore is pending)
models["controller0.gameScore.roundsPlayed"] = 39
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == before + 1 and store.Round == "38", "at round 38 a quit saves: " .. tostring(store.Round))
models["controller0.gameScore.roundsPlayed"] = 41
BO3MacFix.CheckpointMenuCreated("QuitGamePopup", {})
check(#saves == before + 2 and store.Round == "40", "and deeper: " .. tostring(store.Round))
leave_match()
polls(1)
check(state == 0, "the resume undone after the match")

-- the test switches with a Guns line and perks: set up the same way
BO3MacFix.CheckpointMenuCreated = nil
load(nil, "ray_gun_upgraded_zm:2a:0:zm_aat_turned", nil, "specialty_armorvest")
enter_match("zm_theater")
execs, logs, test_loadouts = {}, {}, {}
polls(30)
check(#test_loadouts == 1 and test_loadouts[1].guns == "ray_gun_upgraded_zm:2a:0:zm_aat_turned" and
    test_loadouts[1].perks == "specialty_armorvest", "the test loadout set up")
check(execs[1] == "give bo3macfix_restore", "and given: " .. tostring(execs[1]))

if failures == 0 then
    print("test_checkpoint.lua: all passed")
else
    os.exit(1)
end
