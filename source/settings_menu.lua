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
    local rows = {}
    local description = new_text(self, 64, 600, 1150, 20, 0.8, 0.8, 0.8)
    local notice = new_text(self, 64, 640, 1150, 20, 1, 0.45, 0.3)
    new_text(self, 64, 676, 1150, 18, 0.6, 0.6, 0.6):setText(
        "Up/Down or mouse choose   Left/Right, Cross/Enter or click change   Triangle/R restart the game   Circle/Esc back")
    local focus, first = 1, 1

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
        description:setText(setting and setting.description or "")
        if BO3MacFix.RestartPending() then
            notice:setText("Restart required for your changes to apply.  Press Triangle or R to restart now.")
        end
    end

    local function change(step)
        local setting = settings[focus]
        if setting == nil then return end
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
            BO3MacFix.SetSetting(setting.name, new_value)
            setting.value = new_value
            log("settings menu: " .. setting.name .. " = " .. tostring(new_value))
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
    bind(Enum.LUIButton.LUI_KEY_XBA_PSCROSS, "ENTER", function() change(1) end)
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
