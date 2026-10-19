-- Saving a zombies game to resume it later; checkpoint.c keeps it in BO3MacFix/checkpoint.ini and zm_inventory.c reads
-- and gives the weapons. settings_menu.c loads this after settings_menu.lua in each UI Lua state, which then uses:
-- - BO3MacFix.CheckpointRows(controller, message): the settings page's Save game, Resume saved game and Delete saved
--   game rows;
-- - BO3MacFix.CheckpointMenuCreated(name, menu), for every menu made: quitting a match saves it, and a timer looks after
--   a resumed game (its weapons, and a summary on screen), puts the player's own AAE options back after it, and with
--   AutoSave on saves a solo game at each new round (written on a background queue, checkpoint_autosave.c).
-- What a resume restores:
-- - exactly: the round (AAE's Starting Round, 1 to 255) and the points (Starting Points); the primary weapons, each
--   with its attachments, weapon options (camo) and alternate ammo type, and the perks (the full loadout, Guns and
--   PerkList: zm_inventory.c reads them from the game and gives them back the way the Pack-a-Punch machine does);
-- - from older saves, without the full loadout: the guns by name, Pack-a-Punched ones as their upgraded weapon but
--   without camo, attachments or ammo type, and perks as AAE's Perkaholic (every perk) or Spawn With Quick Revive;
-- - only shown: the scoreboard (kills, headshots, downs, revives), which nothing can set, and the character where the
--   game has zmInventory.player_character_identity (not on Kino, whose HUD gets it from PlayerListZM's data).
-- The arguments are checkpoint.c's functions (settings_menu.c), then test switches, nil normally: save so many seconds
-- into a match; give these weapons first (comma-separated names, or a Guns line); log what the match has so many
-- seconds in. Then AutoSave's writer and deleting the save (checkpoint_autosave.c), and its test switch: how many
-- seconds of a match only set the round it starts at (10). Then zm_inventory.c's full loadout: reading it, setting
-- the checkpoint's up to be given back and what that did, setting a test one up; and its test switch: give these perks
-- first (a PerkList line).
local save_fn, read_fn, apply_fn, clear_fn, resume_fn, weapons_fn, weapon_commands_fn, allow_cheats_fn, notice_fn,
    save_after, give_test, report_after, autosave_fn, delete_fn, autosave_start, loadout_fn, restore_fn,
    restore_result_fn, test_loadout_fn, perks_test = ...
local bo3macfix = BO3MacFix
if bo3macfix == nil or read_fn == nil then return end
bo3macfix.SaveCheckpoint, bo3macfix.ReadCheckpoint, bo3macfix.ApplyCheckpoint = save_fn, read_fn, apply_fn
bo3macfix.ClearCheckpoint, bo3macfix.CheckpointResume, bo3macfix.CheckpointWeapons = clear_fn, resume_fn, weapons_fn
bo3macfix.CheckpointWeaponCommands, bo3macfix.CheckpointAllowCheats = weapon_commands_fn, allow_cheats_fn
bo3macfix.CheckpointNotice = notice_fn
bo3macfix.CheckpointLoadout, bo3macfix.CheckpointRestoreLoadout = loadout_fn, restore_fn
bo3macfix.CheckpointRestoreResult, bo3macfix.CheckpointTestLoadout = restore_result_fn, test_loadout_fn
local log = bo3macfix.Log or function() end

local POLL_MS = 500
-- the options a resume sets (checkpoint.c), whose values in the game ClearCheckpoint compares
local RESUME_DVARS = { "tfoption_starting_round", "tfoption_starting_points", "tfoption_perkaholic",
    "tfoption_spawn_with_quick_res" }
-- the HUD's perks (hudItems.perks.<name>: 1 owned, 2 paused)
local PERKS = { { "juggernaut", "Juggernog" }, { "quick_revive", "Quick Revive" }, { "sleight_of_hand", "Speed Cola" },
    { "doubletap2", "Double Tap" }, { "dead_shot", "Deadshot" }, { "marathon", "Stamin-Up" },
    { "additional_primary_weapon", "Mule Kick" }, { "widows_wine", "Widow's Wine" },
    { "electric_cherry", "Electric Cherry" }, { "phdflopper", "PhD Flopper" }, { "tombstone", "Tombstone" },
    { "whoswho", "Who's Who" }, { "banana_colada", "Banana Colada" }, { "dive_to_nuke", "Dive to Nuke" } }
-- zmInventory.player_character_identity on the maps with Dempsey, Nikolai, Richtofen and Takeo (charindex.gsc's order)
local CHARACTERS = { [0] = "Dempsey", [1] = "Nikolai", [2] = "Richtofen", [3] = "Takeo" }
local OTHER_CREW = { zm_zod = true } -- Shadows of Evil's four aren't those
local QUIT_POPUPS = { QuitGamePopup = true, EndGamePopup = true, QuitPCGamePopup = true }

local function call(fn, ...)
    if type(fn) ~= "function" then return nil end
    local ok, result = pcall(fn, ...)
    if ok then return result end
    return nil
end

local function option(name)
    local value = call(Engine.DvarString, nil, name)
    return value ~= nil and tostring(value) or ""
end

local function current_map()
    local map = call(Engine.GetCurrentMap)
    return type(map) == "string" and map or ""
end

local function in_match()
    local map = current_map()
    return call(Engine.IsInGame) == true and map ~= "" and map ~= "core_frontend"
end

local function model_value(parent, path)
    local model = parent ~= nil and call(Engine.GetModel, parent, path) or nil
    return model ~= nil and call(Engine.GetModelValue, model) or nil
end

local function run_commands(controller, commands)
    for line in string.gmatch(commands, "[^\n]+") do
        Engine.Exec(controller, line)
    end
end

local function read_checkpoint()
    local checkpoint = {}
    for line in string.gmatch(bo3macfix.ReadCheckpoint(), "[^\n]+") do
        local key, value = string.match(line, "^(%a+)=(.*)$")
        if key ~= nil then checkpoint[key] = value end
    end
    return checkpoint
end

-- 12340 as "12,340"
local function number_text(value)
    local text = tostring(value or "")
    local sign, digits = string.match(text, "^(-?)(%d+)$")
    if digits == nil then return text end
    digits = string.reverse((string.gsub(string.reverse(digits), "(%d%d%d)", "%1,")))
    return sign .. (string.gsub(digits, "^,", ""))
end

-- one line of text for checkpoint.ini
local function clean(text)
    return string.sub((string.gsub(tostring(text or ""), "%c", " ")), 1, 200)
end

-- the game's text for a string reference, without the control characters and spaces it has around some (the
-- scoreboard's headers, weapon names)
local function localized(text)
    local result = text ~= nil and text ~= "" and call(Engine.Localize, text) or nil
    result = type(result) == "string" and result ~= "" and result or text
    if type(result) ~= "string" then return result end
    result = string.gsub(result, "%c", "")
    return (string.gsub(string.gsub(result, "^%s+", ""), "%s+$", ""))
end

local function split(list)
    local items = {}
    for item in string.gmatch(list or "", "[^,]+") do table.insert(items, item) end
    return items
end

-- "ray_gun_upgraded" as "ray gun (Pack-a-Punched)"
local function weapon_text(name)
    local base = string.match(name, "^(.-)_upgraded$")
    local text = string.gsub(base or name, "_", " ")
    return base ~= nil and text .. " (Pack-a-Punched)" or text
end

-- the alternate ammo types' names in the game
local AATS = { zm_aat_blast_furnace = "Blast Furnace", zm_aat_dead_wire = "Dead Wire", zm_aat_fire_works = "Fireworks",
    zm_aat_thunder_wall = "Thunder Wall", zm_aat_turned = "Turned" }

-- a Guns line's guns ("name+attachments:options:acvi:aat") as "ray gun (Pack-a-Punched, Turned)"
local function guns_text(guns)
    local texts = {}
    for entry in string.gmatch(guns or "", "[^,]+") do
        local name, rest = string.match(entry, "^([^+:]+)([^:]*):")
        local aat = string.match(entry, ":([%w_]*)$")
        if name ~= nil then
            local text = weapon_text((string.gsub(name, "_zm$", "")))
            local extras = {}
            local _, attachments = string.gsub(rest, "%+", "")
            if attachments > 0 then table.insert(extras, attachments .. (attachments == 1 and " attachment" or " attachments")) end
            if aat ~= nil and aat ~= "" then
                table.insert(extras, AATS[aat] or (string.gsub(string.gsub(aat, "^zm_aat_", ""), "_", " ")))
            end
            if #extras > 0 then
                local upgraded = string.match(text, "^(.-) %(Pack%-a%-Punched%)$")
                text = (upgraded or text) .. " (" .. (upgraded ~= nil and "Pack-a-Punched, " or "") ..
                    table.concat(extras, ", ") .. ")"
            end
            table.insert(texts, text)
        end
    end
    return texts
end

-- whether a checkpoint has the full loadout (Guns and PerkList), which comes back exactly
local function has_loadout(checkpoint)
    return (checkpoint.Guns ~= nil and checkpoint.Guns ~= "") or checkpoint.PerkList ~= nil
end

-- "resume" from when a resumed game's give command for its loadout runs until the game says what it did ("test" for
-- the test switch's): the player holds the starting guns meanwhile
local loadout_pending = nil

local function perk_names(ids)
    local names, wanted = {}, {}
    for _, id in ipairs(split(ids)) do wanted[id] = true end
    for _, perk in ipairs(PERKS) do
        if wanted[perk[1]] then table.insert(names, perk[2]) end
    end
    return names
end

-- the local player's weapons from zm_inventory.c: primaries (what a resume gives back) and the rest
local function weapons_now()
    local text = call(bo3macfix.CheckpointWeapons)
    if type(text) ~= "string" then return nil end
    local primaries, others = {}, {}
    for name, kind in string.gmatch(text, "([%w_]+):(%-?%d+)") do
        -- zombies' weapons are the give command's names with _zm after them (ray_gun_upgraded_zm)
        local weapon = string.gsub(name, "_zm$", "")
        table.insert(tonumber(kind) == 0 and primaries or others, weapon)
    end
    return primaries, others, text
end

-- this game as "Key=value" lines for SaveCheckpoint, and what was read for the log; nil without a round and points
local function collect(controller)
    local root, global = Engine.GetModelForController(controller), call(Engine.GetGlobalModel)
    local client = call(Engine.GetClientNum, controller) or 0
    local player = "PlayerList.client" .. tostring(client)
    -- the HUD's round is one less than gameScore.roundsPlayed (Kino resumed at round 3 drew three tallies with 4 here)
    local played = tonumber(model_value(root, "gameScore.roundsPlayed"))
    local round = played ~= nil and played - 1 or nil
    -- the scoreboard: kills, score (the points), downs, revives and headshots in zombies, named by its own headers
    local stats, points, headers = {}, nil, {}
    for column = 0, 4 do
        local header = call(Engine.GetScoreboardColumnHeader, controller, column)
        local value = call(Engine.GetScoreboardColumnForClient, client, column)
        if type(header) == "string" and header ~= "" and value ~= nil and tostring(value) ~= "" then
            local name = localized(header)
            table.insert(headers, header)
            -- the score is the points, which a resume restores; the rest is what it can't
            if string.find(string.lower(header), "score", 1, true) or string.lower(name) == "score" then
                if points == nil then points = tonumber(value) end
            else
                table.insert(stats, name .. " " .. tostring(value))
            end
        end
    end
    -- the score list's own number for the player, if the scoreboard has none (PlayerList.client<N>.playerScore isn't
    -- the points: it went 1, 3 with the kills)
    if points == nil then
        points = tonumber(model_value(root, player .. ".playerScoreShown") or model_value(global, player .. ".playerScoreShown"))
    end
    if round == nil or round < 1 or points == nil then return nil end
    local perks = {}
    for _, perk in ipairs(PERKS) do
        local value = tonumber(model_value(root, "hudItems.perks." .. perk[1]))
        if value ~= nil and value > 0 then table.insert(perks, perk[1]) end
    end
    local held = model_value(root, "currentWeapon.weaponName")
    local viewmodel = tostring(model_value(root, "currentWeapon.viewmodelWeaponName") or "")
    held = localized(type(held) == "string" and held ~= "" and held or viewmodel)
    if held ~= "" and string.find(string.lower(viewmodel), "upgraded", 1, true) then
        held = held .. " (Pack-a-Punched)"
    end
    local identity = tonumber(model_value(root, "zmInventory.player_character_identity"))
    local icon = model_value(root, player .. ".zombiePlayerIcon") or model_value(global, player .. ".zombiePlayerIcon") or
        model_value(root, player .. ".portraitIcon") or model_value(global, player .. ".portraitIcon")
    local map = current_map()
    local character = identity ~= nil and not OTHER_CREW[map] and CHARACTERS[identity] or nil
    if character == nil and icon ~= nil and tostring(icon) ~= "" then character = tostring(icon) end
    local primaries, others, raw = weapons_now()
    -- each gun with its attachments, options and ammo type, and the perks ("Guns=...\nGunInHand=...\nPerkList=...")
    local loadout = call(bo3macfix.CheckpointLoadout)
    if type(loadout) ~= "string" then loadout = nil end
    -- a resumed game whose saved guns aren't back yet holds the starting ones, or none that can be read: a save then
    -- keeps the saved guns and perks (quitting at once would otherwise swap them for the starting pistol)
    if call(bo3macfix.CheckpointResume, 0) == 2 or loadout_pending == "resume" then
        local checkpoint = read_checkpoint()
        if checkpoint.Map == map and ((checkpoint.Weapons ~= nil and checkpoint.Weapons ~= "") or has_loadout(checkpoint)) then
            primaries, others = split(checkpoint.Weapons), split(checkpoint.Equipment)
            raw = "the saved ones, not given back yet (holding " .. tostring(raw) .. ")"
            if has_loadout(checkpoint) then
                loadout = "Guns=" .. (checkpoint.Guns or "") .. "\nGunInHand=" .. (checkpoint.GunInHand or "") ..
                    "\nPerkList=" .. (checkpoint.PerkList or "")
                perks = split(checkpoint.Perks)
            end
        end
    end
    local lines = { "Map=" .. clean(map), "Round=" .. tostring(math.floor(round)),
        "Points=" .. tostring(math.floor(points)), "Perks=" .. table.concat(perks, ","),
        "Held=" .. clean(held), "Character=" .. clean(character), "Stats=" .. clean(table.concat(stats, ", ")) }
    if primaries ~= nil then
        table.insert(lines, "Weapons=" .. table.concat(primaries, ","))
        table.insert(lines, "Equipment=" .. table.concat(others, ","))
    end
    if loadout ~= nil then table.insert(lines, loadout) end
    local seen = "round " .. tostring(round) .. ", points " .. tostring(points) .. ", perks " ..
        table.concat(perks, ",") .. ", weapons " .. tostring(raw) .. ", held " .. tostring(held) .. " (" ..
        viewmodel .. ", AAT " .. tostring(model_value(root, "currentWeapon.aatIcon")) .. ", PaP level " ..
        tostring(model_value(root, "hudItems.paplv")) .. "), character " .. tostring(identity) .. "/" ..
        tostring(icon) .. ", scoreboard " .. table.concat(stats, ", ") .. " (" .. table.concat(headers, ",") ..
        "), models: roundsPlayed " .. tostring(played) .. ", loadout " .. string.gsub(tostring(loadout), "\n", "; ")
    for _, name in ipairs({ "playerScore", "playerScoreShown", "zombiePlayerIcon", "portraitIcon", "playerName", "clientNum" }) do
        seen = seen .. ", " .. name .. " " .. tostring(model_value(root, player .. "." .. name)) .. "/" ..
            tostring(model_value(global, player .. "." .. name))
    end
    return table.concat(lines, "\n"), seen
end

local function save(controller, why)
    local lines, seen = collect(controller)
    if lines == nil then
        log("checkpoint: couldn't read this game's round and points (" .. why .. ")")
        return nil
    end
    log("checkpoint: saving (" .. why .. "): " .. seen)
    return bo3macfix.SaveCheckpoint(lines)
end

local function summary(checkpoint)
    if checkpoint.Round == nil then return "None saved" end
    local map = checkpoint.Map ~= nil and checkpoint.Map ~= "" and ", " .. checkpoint.Map or ""
    return "Round " .. checkpoint.Round .. ", " .. number_text(checkpoint.Points) .. " points" .. map
end

local function current_options()
    local lines = {}
    for _, dvar in ipairs(RESUME_DVARS) do table.insert(lines, dvar .. "=" .. option(dvar)) end
    return table.concat(lines, "\n")
end

-- puts the player's own starting options back; true if a resume was set up. A resume that can't be undone (a file
-- that can't be read or written) stays on, and the log says so once.
local told_stuck = false
local function clear_resume(controller)
    local commands = bo3macfix.ClearCheckpoint(current_options())
    if commands == nil then
        if not told_stuck and bo3macfix.CheckpointResume(0) ~= 0 then
            told_stuck = true
            log("checkpoint: couldn't undo the resume: BO3MacFix/checkpoint.ini or AAE's mutations file can't be " ..
                "read or written. It stays on, and is tried again.")
        end
        return false
    end
    told_stuck = false
    run_commands(controller, commands)
    return true
end

-- ---- the settings page's rows; message(text) shows what they did

function bo3macfix.CheckpointRows(controller, message)
    local save_row = { kind = "action", category = "Saved game", label = "Save game", live = true,
        text = function() return summary(read_checkpoint()) end }
    save_row.description = function()
        local checkpoint = read_checkpoint()
        if checkpoint.Round == nil then
            return "In a zombies game: saves its round, points, perks and guns to resume later. Quitting saves too, " ..
                "and AutoSave each new round."
        end
        local parts = {}
        local perks = perk_names(checkpoint.Perks)
        if #perks > 0 then table.insert(parts, #perks .. " perks") end
        local weapons = guns_text(checkpoint.Guns)
        if #weapons == 0 then
            for _, name in ipairs(split(checkpoint.Weapons)) do table.insert(weapons, weapon_text(name)) end
        end
        if #weapons > 0 then table.insert(parts, table.concat(weapons, ", ")) end
        if checkpoint.Character ~= nil and checkpoint.Character ~= "" then table.insert(parts, checkpoint.Character) end
        if checkpoint.Stats ~= nil and checkpoint.Stats ~= "" then table.insert(parts, checkpoint.Stats) end
        return #parts > 0 and table.concat(parts, ";  ") or "Saved without perks, guns or scoreboard."
    end
    save_row.run = function()
        if not in_match() then return message("Open this page during a zombies game to save it.") end
        local saved = save(controller, "settings page")
        if saved == nil then return message("Couldn't read this game's round and points.") end
        if not saved then return message("Couldn't save BO3MacFix/checkpoint.ini.") end
        message("Saved " .. summary(read_checkpoint()) .. ".")
    end
    local resume = { kind = "action", category = "Saved game", label = "Resume saved game", live = true,
        description = "Next game: the saved round and points, then your perks and guns back, with their camo, " ..
            "attachments and ammo types. Character and scoreboard can't be restored." }
    resume.text = function()
        local checkpoint = read_checkpoint()
        local state = bo3macfix.CheckpointResume(0)
        if state ~= 0 and checkpoint.AppliedRound ~= nil then
            return (state == 1 and "Next game: round " or "This game: round ") .. checkpoint.AppliedRound
        end
        if state ~= 0 then return "On" end
        return "Off"
    end
    resume.run = function()
        if bo3macfix.CheckpointResume(0) ~= 0 then
            if not clear_resume(controller) then
                return message("Couldn't turn the resume off: BO3MacFix/checkpoint.ini or AAE's mutations file " ..
                    "can't be read or written.")
            end
            return message("Resume off: games start with your own AAE starting options.")
        end
        if in_match() then return message("Set the resume up from the main menu, before starting the game.") end
        local checkpoint = read_checkpoint()
        if checkpoint.Round == nil then return message("Save a game first, during a match.") end
        local commands = bo3macfix.ApplyCheckpoint(bo3macfix.CheckpointPreset())
        if commands == nil then return message("Couldn't set up the resume (see BO3MacFix/log.txt).") end
        run_commands(controller, commands)
        local map = checkpoint.Map ~= nil and checkpoint.Map ~= "" and " Start " .. checkpoint.Map .. " to pick it up." or ""
        message("The next game resumes " .. summary(checkpoint) .. "." .. map)
    end
    -- pressed twice within 5 seconds (by the notices' clock, when there is one)
    local delete = { kind = "action", category = "Saved game", label = "Delete saved game", live = true,
        description = "Deletes the saved game (BO3MacFix/checkpoint.ini) and turns the resume off. Press twice. " ..
            "In a match, AutoSave saves again at the next round." }
    local armed = nil
    local function clock() return tonumber(call(bo3macfix.NoticeClock)) end
    delete.text = function() return armed ~= nil and "Press again to delete" or "" end
    delete.run = function()
        local checkpoint = read_checkpoint()
        if checkpoint.Round == nil then
            armed = nil
            return message("There's no saved game to delete.")
        end
        local now = clock()
        if armed == nil or (now ~= nil and armed.time ~= nil and now - armed.time > 5) then
            armed = { time = now }
            return message("Press again to delete " .. summary(checkpoint) .. ".")
        end
        armed = nil
        -- the player's own AAE options come back first: checkpoint.ini is what has them
        if bo3macfix.CheckpointResume(0) ~= 0 and not clear_resume(controller) then
            return message("Couldn't turn the resume off first: BO3MacFix/checkpoint.ini or AAE's mutations file " ..
                "can't be read or written.")
        end
        if delete_fn == nil or delete_fn() ~= true then
            return message("Couldn't delete BO3MacFix/checkpoint.ini (see BO3MacFix/log.txt).")
        end
        message("Deleted the saved game.")
    end
    return { save_row, resume, delete }
end

-- the preset AAE's custom mutations file is named for (AAECustomMutations_Default.cfg)
function bo3macfix.CheckpointPreset()
    local postfix = CoD ~= nil and call(CoD.GetGameRulePostFix) or nil
    if type(postfix) == "string" and string.match(postfix, "^[%w_ %-]+$") then return postfix end
    return "_Default"
end

-- ---- a match: resuming, test switches, and back in the menus

local match = nil

local function new_match()
    return { polls = 0, weapon_polls = 0, given_at = nil, saved_test = false, reported = false, gave_test = false }
end

-- shows what the resume brought back and what it couldn't, a line at a time
local function show_resume(checkpoint)
    local notice = bo3macfix.CheckpointNotice or function() end
    local perks = perk_names(checkpoint.Perks)
    local perk_text = ""
    if has_loadout(checkpoint) then
        perk_text = "" -- the perks come back with the guns (CheckpointRestoreLoadout)
    elseif #perks > 1 or (#perks == 1 and perks[1] ~= "Quick Revive") then
        perk_text = ", every perk"
    elseif #perks == 1 then
        perk_text = ", Quick Revive"
    end
    notice("Resumed round " .. tostring(checkpoint.AppliedRound or checkpoint.Round) .. " with " ..
        number_text(checkpoint.AppliedPoints or checkpoint.Points) .. " points" .. perk_text)
    local shown = {}
    if checkpoint.Character ~= nil and checkpoint.Character ~= "" then
        table.insert(shown, "character " .. checkpoint.Character)
    end
    if checkpoint.Stats ~= nil and checkpoint.Stats ~= "" then table.insert(shown, checkpoint.Stats) end
    if #shown > 0 then notice("Saved game, not restored: " .. table.concat(shown, ", ")) end
end

-- once the player holds the weapons the game starts with, swaps them for the saved ones
local function give_weapons(controller, checkpoint)
    local primaries = weapons_now()
    if primaries == nil or #primaries == 0 then
        match.weapon_polls = 0
        return
    end
    match.weapon_polls = match.weapon_polls + 1
    -- the starting weapons come one after another (AAE adds the RK5): wait until they've stayed for a few seconds
    if match.weapon_polls < 6 then return end
    bo3macfix.CheckpointResume(2)
    -- the full loadout: zm_inventory.c gives it back on the server thread when the game runs this give command
    if has_loadout(checkpoint) then
        local command = call(bo3macfix.CheckpointRestoreLoadout)
        if type(command) == "string" and command ~= "" then
            log("checkpoint: restoring the saved loadout (" .. tostring(checkpoint.Guns) .. "; perks " ..
                tostring(checkpoint.PerkList) .. ", held " .. table.concat(primaries, ",") .. ")")
            run_commands(controller, command)
            match.restoring_at, loadout_pending = match.polls, "resume"
            return
        end
        log("checkpoint: couldn't set up the saved loadout, giving the guns back by name")
    end
    local commands = bo3macfix.CheckpointWeaponCommands(checkpoint.Weapons or "", table.concat(primaries, ","))
    if commands == nil or commands == "" then
        log("checkpoint: no weapons to give back (saved " .. tostring(checkpoint.Weapons) .. ", held " ..
            table.concat(primaries, ",") .. ")")
        return
    end
    local count = 0
    for _ in string.gmatch(commands, "[^\n]+") do count = count + 1 end
    log("checkpoint: giving the saved weapons back: " .. string.gsub(commands, "\n", "; "))
    bo3macfix.CheckpointAllowCheats(count)
    run_commands(controller, commands)
    match.given_at = match.polls
end

-- ---- AutoSave: each new round of a solo zombies game saves it by itself
-- The first seconds of a match only set the round it starts at (AAE's Starting Round or a resume, which jump to it),
-- and a new round saves once it has stayed for two seconds. Reading the game is all this does on the UI's thread: the
-- writer (checkpoint_autosave.c) saves on a background queue, at most every 15 seconds, and keeps a game saved at a
-- higher round that this one didn't resume.
local AUTOSAVE_START_POLLS = math.floor((tonumber(autosave_start) or 10) * 1000 / POLL_MS)
local AUTOSAVE_SETTLE_POLLS = 4
local AUTOSAVE_AFTER_GIVE_POLLS = 10 -- a resumed game's weapons arrive a moment after the give commands

-- why this game isn't solo, nil if it is: split screen, a recording played back, or more than one client in the game's
-- lobby (a game started with +map may have no lobby, and only a clear answer counts); and the lobby's count, for the log
local function not_solo()
    local lobby = Enum ~= nil and Enum.LobbyType ~= nil and Enum.LobbyType.LOBBY_TYPE_GAME or nil
    local clients = lobby ~= nil and tonumber(call(Engine.GetLobbyClientCount, lobby)) or nil
    local count = tostring(clients) .. " in the game's lobby"
    if call(Engine.IsSplitscreen) == true then return "this one is split screen", count end
    if call(Engine.IsDemoPlaying) == true then return "this one is a recording", count end
    if clients ~= nil and clients > 1 then return "this one has " .. count, count end
    return nil, count
end

local function autosave_tick(controller, round, state)
    if autosave_fn == nil then return end
    local auto = match.autosave
    -- a new match, or the map started over
    if auto == nil or round < auto.round then
        auto = { round = round, polls = 0, next = nil, settled = 0 }
        match.autosave = auto
    end
    auto.polls = auto.polls + 1
    if auto.polls <= AUTOSAVE_START_POLLS or round == auto.round then
        auto.round, auto.next = round, nil
        return
    end
    if auto.next ~= round then auto.next, auto.settled = round, 0 end
    auto.settled = auto.settled + 1
    -- not while a resumed game is getting its weapons back from the saved game
    if auto.settled < AUTOSAVE_SETTLE_POLLS or state == 2 or match.restoring_at ~= nil or
        (match.given_at ~= nil and match.polls < match.given_at + AUTOSAVE_AFTER_GIVE_POLLS) or
        (match.restored_at ~= nil and match.polls < match.restored_at + AUTOSAVE_AFTER_GIVE_POLLS) then
        return
    end
    auto.round, auto.next = round, nil
    if autosave_fn() ~= true or string.sub(current_map(), 1, 3) ~= "zm_" then return end
    local why, players = not_solo()
    if why ~= nil then
        if not auto.told then log("checkpoint: AutoSave is for solo games, and " .. why) end
        auto.told = true
        return
    end
    local lines, seen = collect(controller)
    if lines == nil then
        log("checkpoint: AutoSave couldn't read round " .. tostring(round) .. "'s points")
        return
    end
    autosave_fn(lines, tostring(seen) .. ", " .. players)
end

local function poll(controller)
    if not in_match() then
        match, loadout_pending = nil, nil
        -- back in the menus after a resumed game: the player's own AAE options again (which ends the resume, so
        -- this happens once)
        if bo3macfix.CheckpointResume(0) >= 2 then clear_resume(controller) end
        return
    end
    if match == nil then match = new_match() end
    -- the match's HUD is up once it has a round
    local played = tonumber(model_value(Engine.GetModelForController(controller), "gameScore.roundsPlayed"))
    if played == nil then return end
    match.polls = match.polls + 1
    local state = bo3macfix.CheckpointResume(0)
    if state == 1 then
        state = bo3macfix.CheckpointResume(1)
        show_resume(read_checkpoint())
    end
    if state == 2 and match.polls >= 6 then
        local checkpoint = read_checkpoint()
        if (checkpoint.Weapons == nil or checkpoint.Weapons == "") and not has_loadout(checkpoint) then
            bo3macfix.CheckpointResume(2)
        elseif checkpoint.Map ~= current_map() then
            bo3macfix.CheckpointResume(2)
            log("checkpoint: the saved weapons are " .. tostring(checkpoint.Map) .. "'s, not given on " .. current_map())
        else
            give_weapons(controller, checkpoint)
        end
    end
    if match.given_at ~= nil and match.polls == match.given_at + 6 then
        local primaries, _, raw = weapons_now()
        log("checkpoint: weapons now " .. tostring(raw))
        local notice = bo3macfix.CheckpointNotice or function() end
        local names = {}
        for _, name in ipairs(primaries or {}) do table.insert(names, weapon_text(name)) end
        notice("Guns back: " .. table.concat(names, ", "))
    end
    -- the loadout's restore, which tells what it did once the game has run it
    if match.restoring_at ~= nil then
        local result = call(bo3macfix.CheckpointRestoreResult)
        if type(result) == "string" then
            match.restoring_at, match.restored_at, loadout_pending = nil, match.polls, nil
            local _, _, raw = weapons_now()
            log("checkpoint: loadout restored: " .. result .. "; weapons now " .. tostring(raw))
            local checkpoint = read_checkpoint()
            local notice = bo3macfix.CheckpointNotice or function() end
            local guns = guns_text(checkpoint.Guns)
            if #guns > 0 then notice("Guns back: " .. table.concat(guns, ", ")) end
            local perks = split(checkpoint.PerkList)
            if #perks > 0 then notice("Perks back: " .. #perks) end
            local failed = string.match(result, "failed: (.*)$")
            if failed ~= nil then notice("Not restored: " .. failed) end
        elseif match.polls > match.restoring_at + 20 then
            match.restoring_at, loadout_pending = nil, nil
            log("checkpoint: the saved loadout wasn't restored: the game didn't run its give command")
        end
    end
    -- the HUD's round is one less than roundsPlayed
    autosave_tick(controller, played - 1, state)
    -- test switches
    local seconds = match.polls * POLL_MS / 1000
    if (give_test ~= nil or perks_test ~= nil) and not match.gave_test and seconds >= 12 and
        (string.find(give_test or ":", ":", 1, true) ~= nil) then
        -- a Guns line and a PerkList line: given the way a resume gives the full loadout
        match.gave_test = true
        local command = call(bo3macfix.CheckpointTestLoadout, give_test or "", perks_test or "")
        log("checkpoint test: loadout " .. tostring(give_test) .. "; perks " .. tostring(perks_test) .. ": " ..
            tostring(command))
        if type(command) == "string" and command ~= "" then
            run_commands(controller, command)
            match.restoring_at, loadout_pending = match.polls, "test"
        end
    end
    if give_test ~= nil and not match.gave_test and seconds >= 12 then
        match.gave_test = true
        local commands = bo3macfix.CheckpointWeaponCommands(give_test, "")
        if commands ~= nil and commands ~= "" then
            local count = 0
            for _ in string.gmatch(commands, "[^\n]+") do count = count + 1 end
            log("checkpoint test: " .. string.gsub(commands, "\n", "; "))
            bo3macfix.CheckpointAllowCheats(count)
            run_commands(controller, commands)
        end
    end
    if save_after ~= nil and not match.saved_test and seconds >= (tonumber(save_after) or 30) then
        match.saved_test = true
        log("checkpoint test: saved " .. tostring(save(controller, "BO3MACFIX_CHECKPOINT_SAVE")))
    end
    if report_after ~= nil and not match.reported and seconds >= (tonumber(report_after) or 30) then
        match.reported = true
        local _, seen = collect(controller)
        log("checkpoint test: this match has " .. tostring(seen))
    end
end

local timer_root, errors = nil, 0

local function start_timer()
    if LUI == nil or LUI.roots == nil or LUI.UITimer == nil then return end
    local root = LUI.roots.UIRootFull or LUI.roots.UIRoot0
    if root == nil or root == timer_root then return end
    timer_root = root
    root:addElement(LUI.UITimer.newElementTimer(POLL_MS, false, function()
        local ok, err = pcall(poll, 0)
        if not ok and errors < 5 then
            errors = errors + 1
            log("checkpoint error: " .. tostring(err))
        end
    end))
end

-- settings_menu.lua calls this for every menu made
function bo3macfix.CheckpointMenuCreated(name, menu)
    start_timer()
    -- quitting a match saves it, unless that would replace a game saved at a higher round that this one didn't resume
    if QUIT_POPUPS[name] and in_match() and string.sub(current_map(), 1, 3) == "zm_" then
        local checkpoint = read_checkpoint()
        -- the HUD's round, as a save keeps it: one less than gameScore.roundsPlayed (see collect)
        local played = tonumber(model_value(Engine.GetModelForController(0), "gameScore.roundsPlayed"))
        local round = played ~= nil and played - 1 or nil
        local kept = tonumber(checkpoint.Round)
        if kept ~= nil and round ~= nil and kept > round and bo3macfix.CheckpointResume(0) < 2 then
            log("checkpoint: quitting, kept the game saved at round " .. tostring(kept))
        else
            save(0, "quitting")
        end
    end
end

start_timer()
