-- BO3MacFix's in-game settings page. settings_menu.c runs this in each UI Lua state when its first menu is made.
--
-- The page is a normal menu, BO3MacFixSettings, built like the game's own (CoD.Menu.NewForUIEditor). It opens from a
-- "BO3MACFIX SETTINGS" button in the main menu's list, or from the start menu (Esc/M, or Options on a controller)
-- with Share/Back or F10. Every menu, stock or All-around Enhancement's, goes through CoD.Menu.NewForUIEditor when it's
-- made, and settings_menu.c passes each one to BO3MacFix.MenuCreated, which is where those buttons get added.
-- Settings come from BO3MacFix.GetSettings / SetSetting / RestartGame / RestartPending (settings.c). The argument says
-- to open the page by itself (BO3MACFIX_OPENSETTINGS=1, for testing).
local auto_open = ...
if BO3MacFix == nil or BO3MacFix.GetSettings == nil or LUI == nil or LUI.createMenu == nil or CoD == nil
    or CoD.Menu == nil or CoD.Menu.NewForUIEditor == nil then
    return
end

local PAGE = "BO3MacFixSettings"
local OPEN_BUTTON = Enum.LUIButton.LUI_KEY_BACK
local OPEN_KEY = "F10"
local VISIBLE_ROWS = 22
local log = BO3MacFix.Log or function() end

local function new_text(parent, x, y, width, height, r, g, b)
    local text = LUI.UIText.new()
    text:setLeftRight(true, false, x, x + width)
    text:setTopBottom(true, false, y, y + height)
    text:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_LEFT)
    text:setRGB(r, g, b)
    text:setText("")
    parent:addElement(text)
    return text
end

local function value_text(setting)
    if setting.kind == "action" then
        return setting.text()
    end
    if setting.kind == "toggle" then
        return setting.value ~= 0 and "On" or "Off"
    end
    for i, v in ipairs(setting.values) do
        if v == setting.value then
            return setting.labels[i] or tostring(v)
        end
    end
    return tostring(setting.value)
end

-- ---- the zombies checkpoint (checkpoint.c): the round and points of a game, to start a later one from with All-around
-- Enhancement's Starting Round and Starting Points options, and what else the UI shows about it, for reference
local ROUND_OPTION, POINTS_OPTION = "tfoption_starting_round", "tfoption_starting_points"
local PERKS = { { "juggernaut", "Juggernog" }, { "quick_revive", "Quick Revive" }, { "sleight_of_hand", "Speed Cola" },
    { "doubletap", "Double Tap" }, { "dead_shot", "Deadshot" }, { "marathon", "Stamin-Up" },
    { "additional_primary_weapon", "Mule Kick" }, { "widows_wine", "Widow's Wine" },
    { "electric_cherry", "Electric Cherry" }, { "phdflopper", "PhD Flopper" }, { "tombstone", "Tombstone" },
    { "whoswho", "Who's Who" }, { "banana_colada", "Banana Colada" }, { "dive_to_nuke", "Dive to Nuke" } }

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

local function in_match()
    local map = call(Engine.GetCurrentMap)
    return call(Engine.IsInGame) == true and type(map) == "string" and map ~= "" and map ~= "core_frontend"
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
    for line in string.gmatch(BO3MacFix.ReadCheckpoint(), "[^\n]+") do
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

local function localized(text)
    local result = text ~= nil and text ~= "" and call(Engine.Localize, text) or nil
    return type(result) == "string" and result ~= "" and result or text
end

-- this game's checkpoint as "Key=value" lines, nil without a round
local function collect_checkpoint(controller)
    local root, global = Engine.GetModelForController(controller), call(Engine.GetGlobalModel)
    local client = call(Engine.GetClientNum, controller) or 0
    local round = tonumber(model_value(root, "gameScore.roundsPlayed"))
    local player = "PlayerList.client" .. tostring(client)
    local points = tonumber(model_value(root, player .. ".playerScore") or model_value(global, player .. ".playerScore"))
    -- the scoreboard: score, kills, downs, revives and headshots in zombies, named by its own headers
    local stats = {}
    for column = 0, 4 do
        local header = call(Engine.GetScoreboardColumnHeader, controller, column)
        local value = call(Engine.GetScoreboardColumnForClient, client, column)
        if type(header) == "string" and header ~= "" and value ~= nil and tostring(value) ~= "" then
            table.insert(stats, localized(header) .. " " .. tostring(value))
            if points == nil and column == 0 then points = tonumber(value) end
        end
    end
    if round == nil or round < 1 or points == nil then return nil end
    local perks = {}
    for _, perk in ipairs(PERKS) do
        local value = tonumber(model_value(root, "hudItems.perks." .. perk[1]))
        if value ~= nil and value > 0 then table.insert(perks, perk[2]) end
    end
    local weapon = model_value(root, "currentWeapon.weaponName")
    local viewmodel = tostring(model_value(root, "currentWeapon.viewmodelWeaponName") or "")
    weapon = localized(type(weapon) == "string" and weapon ~= "" and weapon or viewmodel)
    if weapon ~= "" and string.find(string.lower(viewmodel), "upgraded", 1, true) then
        weapon = weapon .. " (Pack-a-Punched)"
    end
    local character = model_value(root, player .. ".zombiePlayerIcon") or model_value(global, player .. ".zombiePlayerIcon")
    return table.concat({ "Map=" .. clean(call(Engine.GetCurrentMap)), "Round=" .. tostring(math.floor(round)),
        "Points=" .. tostring(math.floor(points)), "Perks=" .. clean(table.concat(perks, ", ")),
        "Weapon=" .. clean(weapon), "Character=" .. clean(character), "Stats=" .. clean(table.concat(stats, ", ")) }, "\n")
end

-- the preset AAE's custom mutations file is named for (AAECustomMutations_Default.cfg)
local function mutations_preset()
    local postfix = call(CoD.GetGameRulePostFix)
    if postfix == nil and type(_G) == "table" then postfix = call(rawget(_G, "GetGameRulePostFix")) end
    if type(postfix) == "string" and string.match(postfix, "^[%w_ %-]+$") then return postfix end
    return "_Default"
end

-- once a game has started with a resume set up, back in the menus the player's own starting round and points return
local resume_checked = false
local function check_resume()
    if resume_checked or BO3MacFix.CheckpointResume == nil then return end
    local state = BO3MacFix.CheckpointResume(0)
    if in_match() then
        resume_checked = true
        if state == 1 then BO3MacFix.CheckpointResume(1) end
    elseif state == 2 then
        resume_checked = true
        local commands = BO3MacFix.ClearCheckpoint(option(ROUND_OPTION), option(POINTS_OPTION))
        if commands ~= nil then run_commands(0, commands) end
    end
end

-- the page's two checkpoint rows; message(text) shows what they did
local function checkpoint_rows(controller, message)
    local function summary(checkpoint)
        if checkpoint.Round == nil then return "None saved" end
        local map = checkpoint.Map ~= nil and checkpoint.Map ~= "" and ", " .. checkpoint.Map or ""
        return "Round " .. checkpoint.Round .. ", " .. number_text(checkpoint.Points) .. " points" .. map
    end
    local save = { kind = "action", category = "Checkpoint", label = "Save checkpoint", live = true,
        text = function() return summary(read_checkpoint()) end }
    save.description = function()
        local checkpoint = read_checkpoint()
        if checkpoint.Round == nil then
            return "In a game: saves its round and points (and perks, weapon and scoreboard to look at) for later."
        end
        local parts = {}
        for _, key in ipairs({ "Perks", "Weapon", "Stats" }) do
            if checkpoint[key] ~= nil and checkpoint[key] ~= "" then table.insert(parts, checkpoint[key]) end
        end
        return #parts > 0 and table.concat(parts, "   ") or "Saved without perks, weapon or scoreboard."
    end
    save.run = function()
        if not in_match() then return message("Open this page during a zombies game to save it.") end
        local lines = collect_checkpoint(controller)
        if lines == nil then return message("Couldn't read this game's round and points.") end
        if not BO3MacFix.SaveCheckpoint(lines) then return message("Couldn't save BO3MacFix/checkpoint.ini.") end
        message("Saved " .. summary(read_checkpoint()) .. ".")
    end
    local resume = { kind = "action", category = "Checkpoint", label = "Resume it in the next game", live = true,
        description = "Starts the next game at the saved round with the saved points (AAE's Starting Round and " ..
            "Starting Points). Weapons and perks start as usual." }
    resume.text = function()
        local checkpoint = read_checkpoint()
        if BO3MacFix.CheckpointResume(0) ~= 0 and checkpoint.AppliedRound ~= nil then
            return "Next game: round " .. checkpoint.AppliedRound .. ", " .. number_text(checkpoint.AppliedPoints)
        end
        return "Off"
    end
    resume.run = function()
        if BO3MacFix.CheckpointResume(0) ~= 0 then
            local commands = BO3MacFix.ClearCheckpoint(option(ROUND_OPTION), option(POINTS_OPTION))
            if commands ~= nil then run_commands(controller, commands) end
            return message("Resume off: games start with your own Starting Round and Starting Points.")
        end
        if in_match() then return message("Set the resume up from the main menu, before starting the game.") end
        local checkpoint = read_checkpoint()
        if checkpoint.Round == nil then return message("Save a checkpoint during a game first.") end
        local commands = BO3MacFix.ApplyCheckpoint(mutations_preset(), option(ROUND_OPTION), option(POINTS_OPTION))
        if commands == nil then return message("Couldn't set up the resume (see BO3MacFix/log.txt).") end
        run_commands(controller, commands)
        local map = checkpoint.Map ~= nil and checkpoint.Map ~= "" and " Start " .. checkpoint.Map .. " to pick it up." or ""
        local applied = read_checkpoint()
        message("The next game starts at round " .. tostring(applied.AppliedRound) .. " with " ..
            number_text(applied.AppliedPoints) .. " points." .. map)
    end
    return { save, resume }
end

local function build_page(controller)
    local self = CoD.Menu.NewForUIEditor(PAGE)
    self.soundSet = "default"
    self:setOwner(controller)
    self:setLeftRight(true, true, 0, 0)
    self:setTopBottom(true, true, 0, 0)
    self.buttonModel = Engine.CreateModel(Engine.GetModelForController(controller), PAGE .. ".buttonPrompts")

    local shade = LUI.UIImage.new()
    shade:setLeftRight(true, true, 0, 0)
    shade:setTopBottom(true, true, 0, 0)
    shade:setRGB(0.02, 0.02, 0.03)
    shade:setAlpha(0.92)
    self:addElement(shade)

    new_text(self, 64, 28, 1150, 34, 1, 0.75, 0.2):setText("BO3MacFix")
    new_text(self, 64, 64, 1150, 18, 0.7, 0.7, 0.7):setText("Settings are saved to BO3MacFix/settings.ini")

    local settings = BO3MacFix.GetSettings()
    -- what a checkpoint row last did, shown until another row changes something
    local checkpoint_message = nil
    if BO3MacFix.ReadCheckpoint ~= nil then
        for _, row in ipairs(checkpoint_rows(controller, function(text) checkpoint_message = text end)) do
            table.insert(settings, row)
        end
    end
    local rows = {}
    local description = new_text(self, 64, 600, 1150, 20, 0.8, 0.8, 0.8)
    local notice = new_text(self, 64, 640, 1150, 20, 1, 0.45, 0.3)
    new_text(self, 64, 676, 1150, 18, 0.6, 0.6, 0.6):setText(
        "Up/Down or mouse choose   Left/Right, Cross/Enter or click change   Triangle/R restart the game   Circle/Esc back")
    -- save_failed: the label of a setting settings.ini couldn't take, shown until a change is saved
    local focus, first, save_failed = 1, 1, nil

    local function refresh()
        if focus < first then first = focus end
        if focus > first + VISIBLE_ROWS - 1 then first = focus - VISIBLE_ROWS + 1 end
        local last_category = nil
        for i = 1, VISIBLE_ROWS do
            local index, row = first + i - 1, rows[i]
            local setting = settings[index]
            if setting == nil then
                row.category:setText("") row.label:setText("") row.value:setText("")
            else
                local selected = index == focus
                row.category:setText(setting.category ~= last_category and setting.category or "")
                last_category = setting.category
                row.label:setText((selected and "> " or "  ") .. setting.label .. (setting.live and "" or "  (restart)"))
                row.value:setText(value_text(setting))
                if selected then
                    row.label:setRGB(1, 0.78, 0.2) row.value:setRGB(1, 0.78, 0.2)
                else
                    row.label:setRGB(0.88, 0.88, 0.88) row.value:setRGB(1, 1, 1)
                end
            end
        end
        local setting = settings[focus]
        local about = setting and setting.description or ""
        if type(about) == "function" then about = about() end
        description:setText(about)
        if checkpoint_message ~= nil then
            notice:setText(checkpoint_message)
        elseif save_failed ~= nil then
            notice:setText("Couldn't save " .. save_failed .. " to BO3MacFix/settings.ini.")
        elseif BO3MacFix.RestartPending() then
            notice:setText("Restart required for your changes to apply.  Press Triangle or R to restart now.")
        else
            notice:setText("")
        end
    end

    -- pressed: Cross/Enter or a click, the only way to run a checkpoint row
    local function change(step, pressed)
        local setting = settings[focus]
        if setting == nil then return end
        if setting.kind == "action" then
            if pressed then
                local ok, err = pcall(setting.run)
                if not ok then
                    checkpoint_message = "That didn't work (see BO3MacFix/log.txt)."
                    log("settings menu error: " .. tostring(err))
                end
            end
            refresh()
            return
        end
        checkpoint_message = nil
        local new_value = setting.value
        if setting.kind == "toggle" then
            new_value = setting.value ~= 0 and 0 or 1
        elseif #setting.values > 0 then
            local at = 1
            for i, v in ipairs(setting.values) do
                if v == setting.value then at = i end
            end
            at = (at - 1 + step) % #setting.values + 1
            new_value = setting.values[at]
        end
        if new_value ~= setting.value then
            -- nil when settings.ini couldn't be written: then the page keeps showing the value in effect
            if BO3MacFix.SetSetting(setting.name, new_value) == nil then
                save_failed = setting.label
                log("settings menu: couldn't save " .. setting.name .. " = " .. tostring(new_value))
            else
                save_failed = nil
                setting.value = new_value
                log("settings menu: " .. setting.name .. " = " .. tostring(new_value))
            end
        end
        refresh()
    end

    for i = 1, VISIBLE_ROWS do
        local y = 96 + (i - 1) * 22
        -- hovering the mouse over a row picks it; a left click arrives as Cross/A, which changes it
        local hit = LUI.UIElement.new()
        hit:setLeftRight(true, false, 240, 1200)
        hit:setTopBottom(true, false, y, y + 21)
        hit:setHandleMouse(true)
        hit:registerEventHandler("mouseenter", function()
            if settings[first + i - 1] ~= nil then focus = first + i - 1 refresh() end
            return true
        end)
        self:addElement(hit)
        rows[i] = { category = new_text(self, 64, y, 170, 19, 0.45, 0.7, 1),
                    label = new_text(self, 240, y, 620, 19, 0.9, 0.9, 0.9),
                    value = new_text(self, 870, y, 330, 19, 1, 1, 1) }
    end

    local function bind(button, key, action)
        self:AddButtonCallbackFunction(self, controller, button, key, function(element, menu, controller, model)
            action(menu, controller)
            return true
        end, function() return false end, false)
    end
    bind(Enum.LUIButton.LUI_KEY_UP, "UPARROW", function() focus = math.max(1, focus - 1) refresh() end)
    bind(Enum.LUIButton.LUI_KEY_DOWN, "DOWNARROW", function() focus = math.min(#settings, focus + 1) refresh() end)
    bind(Enum.LUIButton.LUI_KEY_LEFT, "LEFTARROW", function() change(-1) end)
    bind(Enum.LUIButton.LUI_KEY_RIGHT, "RIGHTARROW", function() change(1) end)
    bind(Enum.LUIButton.LUI_KEY_XBA_PSCROSS, "ENTER", function() change(1, true) end)
    bind(Enum.LUIButton.LUI_KEY_XBY_PSTRIANGLE, "R", function()
        if BO3MacFix.RestartPending() then
            log("settings menu: restarting the game")
            BO3MacFix.RestartGame()
        end
    end)
    -- the game already sends Esc as Circle/B
    bind(Enum.LUIButton.LUI_KEY_XBB_PSCIRCLE, nil, function(menu, controller) GoBack(menu, controller) end)
    LUI.OverrideFunction_CallOriginalSecond(self, "close", function()
        Engine.UnsubscribeAndFreeModel(Engine.GetModel(Engine.GetModelForController(controller), PAGE .. ".buttonPrompts"))
    end)

    refresh()
    log("settings menu: opened with " .. #settings .. " settings")
    return self
end

-- an error while making a menu leaves the game's Lua garbage collector stopped, so if the page can't be built, this
-- makes a bare one that Circle/Esc can still leave
LUI.createMenu[PAGE] = function(controller)
    local ok, page = pcall(build_page, controller)
    if ok and page ~= nil then return page end
    log("settings menu error: " .. tostring(page))
    local self = CoD.Menu.NewForUIEditor(PAGE)
    self:setOwner(controller)
    self:setLeftRight(true, true, 0, 0)
    self:setTopBottom(true, true, 0, 0)
    self.buttonModel = Engine.CreateModel(Engine.GetModelForController(controller), PAGE .. ".buttonPrompts")
    self:AddButtonCallbackFunction(self, controller, Enum.LUIButton.LUI_KEY_XBB_PSCIRCLE, nil, function(element, menu, controller)
        GoBack(menu, controller)
        return true
    end, function() return false end, false)
    return self
end

-- opens the page over menu, the way the game's own menus open theirs
function BO3MacFix.OpenSettings(menu, controller)
    OpenOverlay(menu, PAGE, controller or menu:getOwner() or 0)
end

-- runs fn after the given time, from a timer element (if element closes first, the timer goes with it)
local function after(element, milliseconds, fn)
    element:addElement(LUI.UITimer.newElementTimer(milliseconds, true, function()
        local ok, err = pcall(fn)
        if not ok then log("settings menu error: " .. tostring(err)) end
    end))
end

local function add_start_menu_button(menu)
    menu:AddButtonCallbackFunction(menu, menu:getOwner() or 0, OPEN_BUTTON, OPEN_KEY,
        function(element, menu, controller, model)
            BO3MacFix.OpenSettings(menu, controller)
            return true
        end, function(element, menu, controller)
            CoD.Menu.SetButtonLabel(menu, OPEN_BUTTON, "BO3MacFix")
            return true
        end, false)
    log("settings menu: added the BO3MacFix button to the start menu")
end

-- A "BO3MACFIX SETTINGS" button in the main menu's list (just above Quit) and the zombies lobbies'. Those lists come
-- from CoD.LobbyMenus.AddButtonsForTarget each time they're built, so the button goes into what it returns. All-around
-- Enhancement replaces the whole CoD.LobbyMenus table, so this is checked again whenever a menu is made, before the
-- menu builds its list.
local ENTRY_TARGETS = { "UI_MAIN", "UI_MODESELECT", "UI_ZMLOBBYONLINE", "UI_ZMLOBBYLANGAME" }
local ENTRY_ID = "btnBO3MacFix"
local entry_add_buttons = nil

local function wants_entry(id)
    local targets = LobbyData and LobbyData.UITargets
    if targets == nil then return false end
    for _, name in ipairs(ENTRY_TARGETS) do
        if targets[name] ~= nil and targets[name].id == id then return true end
    end
    return false
end

local function add_entry(buttons)
    for _, button in ipairs(buttons) do
        if button.customId == ENTRY_ID then return end
    end
    local at = #buttons + 1
    local quit = CoD.LobbyButtons and CoD.LobbyButtons.QUIT
    if quit ~= nil then
        for i, button in ipairs(buttons) do
            if button.optionDisplay == quit.stringRef then at = i break end
        end
    end
    table.insert(buttons, at, { optionDisplay = "BO3MACFIX SETTINGS", customId = ENTRY_ID,
        -- list buttons are called with (self, element, controller, param, menu)
        action = function(self, element, controller, param, menu) BO3MacFix.OpenSettings(menu or self, controller) end,
        isLargeButton = false, isLastButtonInGroup = false, disabled = false, selected = false,
        isBreadcrumbNew = false, warning = false })
end

local function ensure_lobby_entry()
    local menus = CoD.LobbyMenus
    local add_buttons = menus and menus.AddButtonsForTarget
    if type(add_buttons) ~= "function" or add_buttons == entry_add_buttons then return end
    entry_add_buttons = function(controller, id, ...)
        local buttons = add_buttons(controller, id, ...)
        if type(buttons) == "table" and wants_entry(id) then
            local ok, err = pcall(add_entry, buttons)
            if not ok then log("settings menu error: " .. tostring(err)) end
        end
        return buttons
    end
    menus.AddButtonsForTarget = entry_add_buttons
end

-- settings_menu.c calls this for every menu made after the page has loaded, right after CoD.Menu.NewForUIEditor and
-- before the menu's constructor goes on
local opened_once = false
function BO3MacFix.MenuCreated(name, menu)
    ensure_lobby_entry()
    check_resume()
    if name == "StartMenu_Main" then
        -- the start menu's constructor sets it up after this returns, so add the button once it has
        after(menu, 1, function() add_start_menu_button(menu) end)
    end
    if auto_open and not opened_once and name ~= PAGE then
        after(menu, 5000, function()
            if opened_once then return end
            opened_once = true
            log("settings menu: opening by itself (BO3MACFIX_OPENSETTINGS) over " .. tostring(name))
            BO3MacFix.OpenSettings(menu)
        end)
    end
end
ensure_lobby_entry()
log("settings menu: ready (main menu button, or Share/Back or F10 in the start menu)")
