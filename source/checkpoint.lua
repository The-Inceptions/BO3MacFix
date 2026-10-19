-- Saving a zombies game to resume it later; checkpoint.c keeps it in BO3MacFix/checkpoint.ini and zm_inventory.c reads
-- and gives the weapons. settings_menu.c loads this after settings_menu.lua in each UI Lua state, which then uses:
-- - BO3MacFix.CheckpointRows(controller, message): the settings page's Save game and Resume saved game rows;
-- - BO3MacFix.CheckpointMenuCreated(name, menu), for every menu made: quitting a match saves it, and a timer looks after
--   a resumed game (its weapons, and a summary on screen) and puts the player's own AAE options back after it.
-- What a resume restores:
-- - exactly: the round (AAE's Starting Round, 1 to 255) and the points (Starting Points);
-- - partly: perks, as AAE's Perkaholic (every perk) or Spawn With Quick Revive (that one alone); the primary weapons,
--   given back by name, Pack-a-Punched ones as their upgraded weapon but without camo, attachments or ammo type;
-- - only shown: the scoreboard (kills, headshots, downs, revives), which nothing can set, and the character where the
--   game has zmInventory.player_character_identity (not on Kino, whose HUD gets it from PlayerListZM's data).
-- The arguments are checkpoint.c's functions (settings_menu.c), then test switches, nil normally: save so many seconds
-- into a match; give these weapons (comma-separated) first; log what the match has so many seconds in.
local save_fn, read_fn, apply_fn, clear_fn, resume_fn, weapons_fn, weapon_commands_fn, allow_cheats_fn, notice_fn,
    save_after, give_test, report_after = ...
local bo3macfix = BO3MacFix
if bo3macfix == nil or read_fn == nil then return end
bo3macfix.SaveCheckpoint, bo3macfix.ReadCheckpoint, bo3macfix.ApplyCheckpoint = save_fn, read_fn, apply_fn
bo3macfix.ClearCheckpoint, bo3macfix.CheckpointResume, bo3macfix.CheckpointWeapons = clear_fn, resume_fn, weapons_fn
bo3macfix.CheckpointWeaponCommands, bo3macfix.CheckpointAllowCheats = weapon_commands_fn, allow_cheats_fn
bo3macfix.CheckpointNotice = notice_fn
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
    -- a resumed game whose saved guns aren't back yet holds the starting ones, or none that can be read: a save then
    -- keeps the saved guns (quitting at once would otherwise swap them for the starting pistol)
    if call(bo3macfix.CheckpointResume, 0) == 2 then
        local checkpoint = read_checkpoint()
        if checkpoint.Map == map and checkpoint.Weapons ~= nil and checkpoint.Weapons ~= "" then
            primaries, others = split(checkpoint.Weapons), split(checkpoint.Equipment)
            raw = "the saved ones, not given back yet (holding " .. tostring(raw) .. ")"
        end
    end
    local lines = { "Map=" .. clean(map), "Round=" .. tostring(math.floor(round)),
        "Points=" .. tostring(math.floor(points)), "Perks=" .. table.concat(perks, ","),
        "Held=" .. clean(held), "Character=" .. clean(character), "Stats=" .. clean(table.concat(stats, ", ")) }
    if primaries ~= nil then
        table.insert(lines, "Weapons=" .. table.concat(primaries, ","))
        table.insert(lines, "Equipment=" .. table.concat(others, ","))
    end
    local seen = "round " .. tostring(round) .. ", points " .. tostring(points) .. ", perks " ..
        table.concat(perks, ",") .. ", weapons " .. tostring(raw) .. ", held " .. tostring(held) .. " (" ..
        viewmodel .. ", AAT " .. tostring(model_value(root, "currentWeapon.aatIcon")) .. ", PaP level " ..
        tostring(model_value(root, "hudItems.paplv")) .. "), character " .. tostring(identity) .. "/" ..
        tostring(icon) .. ", scoreboard " .. table.concat(stats, ", ") .. " (" .. table.concat(headers, ",") ..
        "), models: roundsPlayed " .. tostring(played)
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

-- puts the player's own starting options back; true if a resume was set up
local function clear_resume(controller)
    local commands = bo3macfix.ClearCheckpoint(current_options())
    if commands == nil then return false end
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
            return "In a zombies game: saves its round, points, perks and guns to resume later. Quitting saves too."
        end
        local parts = {}
        local perks = perk_names(checkpoint.Perks)
        if #perks > 0 then table.insert(parts, #perks .. " perks") end
        local weapons = {}
        for _, name in ipairs(split(checkpoint.Weapons)) do table.insert(weapons, weapon_text(name)) end
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
        description = "Next game: the saved round and points, every perk (AAE's Perkaholic) if you had perks, and " ..
            "your guns back (upgraded, without camo or ammo type). Character and scoreboard can't be restored." }
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
            clear_resume(controller)
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
    return { save_row, resume }
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
    if #perks > 1 or (#perks == 1 and perks[1] ~= "Quick Revive") then
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
    local commands = bo3macfix.CheckpointWeaponCommands(checkpoint.Weapons or "", table.concat(primaries, ","))
    bo3macfix.CheckpointResume(2)
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

local function poll(controller)
    if not in_match() then
        match = nil
        -- back in the menus after a resumed game: the player's own AAE options again (which ends the resume, so
        -- this happens once)
        if bo3macfix.CheckpointResume(0) >= 2 then clear_resume(controller) end
        return
    end
    if match == nil then match = new_match() end
    -- the match's HUD is up once it has a round
    if tonumber(model_value(Engine.GetModelForController(controller), "gameScore.roundsPlayed")) == nil then return end
    match.polls = match.polls + 1
    local state = bo3macfix.CheckpointResume(0)
    if state == 1 then
        state = bo3macfix.CheckpointResume(1)
        show_resume(read_checkpoint())
    end
    if state == 2 and match.polls >= 6 then
        local checkpoint = read_checkpoint()
        if checkpoint.Weapons == nil or checkpoint.Weapons == "" then
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
    -- test switches
    local seconds = match.polls * POLL_MS / 1000
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
