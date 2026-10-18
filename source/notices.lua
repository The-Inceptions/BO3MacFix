-- BO3MacFix's on-screen notices and PauseOnDisconnect, the UI half of notices.m. The first menu made in each UI Lua
-- state loads this. A timer on the UI's root then looks four times a second for a notice to show (one at a time,
-- each for four seconds at the top of the screen, over menus and the HUD) and for a request to pause because a
-- controller disconnected. The argument says to log every menu made (BO3MACFIX_NOTICESDEBUG=1).
local debug_menus = ...
local bo3macfix = BO3MacFix
if bo3macfix == nil or bo3macfix.TakeNotice == nil or LUI == nil or LUI.roots == nil or LUI.UITimer == nil then
    return
end
local log = bo3macfix.NoticeLog or function() end
local take_notice, take_pause, pause_game = bo3macfix.TakeNotice, bo3macfix.TakePauseRequest, bo3macfix.PauseGame

local POLL_MS, SHOW_MS, FADE_MS = 250, 4000, 500
-- in the UI's 1280x720 space: centred, below the MacBook's camera housing
local WIDTH, TOP, HEIGHT = 640, 34, 34

local function ui_root()
    return LUI.roots.UIRootFull or LUI.roots.UIRoot0
end

local toast, toast_text, toast_ms, fading = nil, nil, 0, false
-- a menu was made while a toast showed: it goes on the root after the toast, so the toast is put back on top
local raise = false

local function close_toast()
    local old = toast
    toast = nil
    if old ~= nil then old:close() end
end

local function show(text)
    close_toast()
    local root = ui_root()
    if root == nil then return end
    local box = LUI.UIElement.new()
    box:setLeftRight(false, false, -WIDTH / 2, WIDTH / 2)
    box:setTopBottom(true, false, TOP, TOP + HEIGHT)
    local back = LUI.UIImage.new()
    back:setLeftRight(true, true, 0, 0)
    back:setTopBottom(true, true, 0, 0)
    back:setRGB(0.02, 0.02, 0.03)
    back:setAlpha(0.82)
    box:addElement(back)
    local accent = LUI.UIImage.new()
    accent:setLeftRight(true, false, 0, 4)
    accent:setTopBottom(true, true, 0, 0)
    accent:setRGB(1, 0.75, 0.2)
    box:addElement(accent)
    local label = LUI.UIText.new()
    label:setLeftRight(true, true, 14, -10)
    label:setTopBottom(false, false, -10, 10)
    label:setAlignment(Enum.LUIAlignment.LUI_ALIGNMENT_CENTER)
    label:setRGB(1, 1, 1)
    label:setText(text)
    box:addElement(label)
    -- added last, so it's drawn over everything already on the root
    root:addElement(box)
    toast, toast_text, toast_ms, fading = box, text, 0, false
end

local function fade_out()
    fading = true
    toast:beginAnimation("fade_out", FADE_MS)
    toast:setAlpha(0)
end

-- the pause menu (StartMenu_Main) is open, from NoticesMenuCreated
local pause_menu_open = false

-- an Engine function's answer as true or false (some return 0 or 1), nil if there's no such function or it failed
local function ask(fn, ...)
    if type(fn) ~= "function" then return nil end
    local ok, result = pcall(fn, ...)
    if not ok or result == nil then return nil end
    if type(result) == "number" then return result ~= 0 end
    return result and true or false
end

local function try_pause(forced)
    local in_game, can_pause, gamepad = ask(Engine.IsInGame), ask(Engine.CanPauseGame), ask(Engine.LastInput_Gamepad, 0)
    local why = nil
    if in_game ~= true then
        why = "not in a match"
    elseif can_pause == false then
        why = "this game can't pause"
    elseif pause_menu_open then
        why = "the pause menu is open already"
    elseif gamepad == false and not forced then
        why = "the last input came from the keyboard or mouse"
    end
    log("PauseOnDisconnect: in a match " .. tostring(in_game) .. ", can pause " .. tostring(can_pause) ..
        ", last input a controller " .. tostring(gamepad) .. (why and (", not pausing: " .. why) or ", pausing"))
    if why == nil then pause_game() end
end

local function poll()
    if raise and toast ~= nil and not fading then
        local shown_ms = toast_ms
        show(toast_text)
        toast_ms = shown_ms
    end
    raise = false
    if toast ~= nil then
        toast_ms = toast_ms + POLL_MS
        if toast_ms >= SHOW_MS + FADE_MS then
            close_toast()
        elseif toast_ms >= SHOW_MS and not fading and not pcall(fade_out) then
            close_toast()
        end
    end
    if toast == nil then
        local text = take_notice()
        if text ~= nil then
            local ok, err = pcall(show, text)
            if ok then
                log("notices: showing " .. text)
            else
                pcall(close_toast)
                log("notices error: " .. tostring(err))
            end
        end
    end
    local forced = take_pause()
    if forced ~= nil then try_pause(forced) end
end

local timer_root, errors = nil, 0

local function start_timer()
    local root = ui_root()
    if root == nil or root == timer_root then return end
    timer_root = root
    root:addElement(LUI.UITimer.newElementTimer(POLL_MS, false, function()
        local ok, err = pcall(poll)
        if not ok and errors < 5 then
            errors = errors + 1
            log("notices error: " .. tostring(err))
        end
    end))
    log("notices: ready on " .. (root == LUI.roots.UIRootFull and "UIRootFull" or "UIRoot0"))
end

-- notices.m's start chunk calls this for every menu made after this file has loaded
function bo3macfix.NoticesMenuCreated(name, menu)
    start_timer()
    raise = toast ~= nil
    if name == "StartMenu_Main" then
        pause_menu_open = true
        LUI.OverrideFunction_CallOriginalSecond(menu, "close", function() pause_menu_open = false end)
    end
    if debug_menus then log("notices: menu " .. tostring(name)) end
end

start_timer()
