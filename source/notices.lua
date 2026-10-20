-- BO3MacFix's on-screen notices and PauseOnDisconnect, the UI half of notices.m, and whether the pause menu is open
-- in a match for PauseMenuFPS. The first menu made in each UI Lua state loads this. A timer then looks four times a
-- second for a notice to show (one at a time, each for four seconds at the top of the screen, a warning for eight),
-- for a request to pause because a controller disconnected, and at the pause menu. The argument says to log every
-- menu made (BO3MACFIX_NOTICESDEBUG=1).
local debug_menus = ...
local bo3macfix = BO3MacFix
if bo3macfix == nil or bo3macfix.TakeNotice == nil or LUI == nil or LUI.roots == nil or LUI.UITimer == nil then
    return
end
local log = bo3macfix.NoticeLog or function() end
local take_notice, take_pause, pause_game = bo3macfix.TakeNotice, bo3macfix.TakePauseRequest, bo3macfix.PauseGame
local tell_pause_menu = bo3macfix.PauseMenuOpen or function() end
local clock = bo3macfix.NoticeClock -- seconds

local POLL_MS, SHOW, FADE = 250, 4.0, 0.5
-- in the UI's 1280x720 space: centred, below the MacBook's camera housing; wider for a long text (about 10 units a
-- character at this height), up to most of the screen
local WIDTH, MAX_WIDTH, CHAR_WIDTH, TOP, HEIGHT = 640, 1200, 10, 34, 34

local function root_name(element)
    for name, root in pairs(LUI.roots) do
        if root == element then return tostring(name) end
    end
    return nil
end

-- the root an element is on, nil while it isn't on one
local function root_of(element)
    local top, parent = element, element:getParent()
    while parent ~= nil do
        top, parent = parent, parent:getParent()
    end
    if top ~= element and root_name(top) ~= nil then return top end
    return nil
end

-- Toasts go on the root the latest menu went on, after it, so they're drawn over it: in a match the HUD and the
-- pause menu are on a root that's drawn over UIRootFull
local toast_root = nil
local function ui_root()
    return toast_root or LUI.roots.UIRootFull or LUI.roots.UIRoot0
end

local toast, toast_text, toast_shown, fading = nil, nil, 0, false
-- how long the toast shows: SHOW, or what TakeNotice said for a lasting one (a warning)
local toast_seconds = SHOW

local function close_toast()
    local old = toast
    toast = nil
    if old ~= nil then old:close() end
end

local function show(text, seconds)
    close_toast()
    local root = ui_root()
    if root == nil then return end
    local box = LUI.UIElement.new()
    local width = math.max(WIDTH, math.min(MAX_WIDTH, #text * CHAR_WIDTH + 48))
    box:setLeftRight(false, false, -width / 2, width / 2)
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
    toast, toast_text, toast_shown, fading = box, text, clock(), false
    toast_seconds = seconds or SHOW
end

local function fade_out()
    fading = true
    toast:beginAnimation("fade_out", FADE * 1000)
    toast:setAlpha(0)
end

-- the pause menu (StartMenu_Main) from NoticesMenuCreated until it closes, and whether it has been on a root yet
local pause_menu, pause_menu_shown = nil, false
-- whether BO3MacFix was last told the pause menu is open in a match
local told_open = false

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
    elseif pause_menu ~= nil then
        why = "the pause menu is open already"
    elseif gamepad == false and not forced then
        why = "the last input came from the keyboard or mouse"
    end
    log("PauseOnDisconnect: in a match " .. tostring(in_game) .. ", can pause " .. tostring(can_pause) ..
        ", last input a controller " .. tostring(gamepad) .. (why and (", not pausing: " .. why) or ", pausing"))
    if why == nil then pause_game() end
end

local function pause_menu_closed(menu)
    if menu ~= pause_menu then return end -- an older one
    pause_menu = nil
    if told_open then
        told_open = false
        tell_pause_menu(0)
    end
end

-- PauseMenuFPS: while the pause menu is on a root in a match, BO3MacFix is told so on every poll (it stops counting it
-- as open 2 seconds after the last time, so a UI state that goes away with the menu open, as when a map loads, doesn't
-- leave the limit on), and once when that ends: the menu closed, or the match ended. A pause menu that has left its
-- root without its close running (the UI closing everything at once) counts as closed.
local function check_pause_menu()
    local open = false
    if pause_menu ~= nil then
        if root_of(pause_menu) ~= nil then
            pause_menu_shown = true
            open = ask(Engine.IsInGame) == true
        elseif pause_menu_shown then
            pause_menu_closed(pause_menu)
        end
    end
    if open or told_open then tell_pause_menu(open and 1 or 0) end
    told_open = open
end

-- the latest menu made, until it's on a root (it goes on one after it's made, in the same frame)
local new_menu, new_menu_name, new_menu_looks = nil, nil, 0

-- BO3MACFIX_NOTICESDEBUG: whether the toast is the last element on its root, so drawn over everything on it
local function log_order(what)
    if not debug_menus or toast == nil then return end
    local root = ui_root()
    log("notices: " .. what .. " on " .. tostring(root_name(root)) .. ", the last element there: " ..
        tostring(root:getLastChild() == toast))
end

local function poll()
    if new_menu ~= nil then
        local root = root_of(new_menu)
        new_menu_looks = new_menu_looks + 1
        if root ~= nil or new_menu_looks >= 4 then
            if debug_menus then
                log("notices: menu " .. tostring(new_menu_name) .. " is on " .. tostring(root and root_name(root)) ..
                    ", priority " .. tostring(new_menu:getPriority()))
            end
            new_menu = nil
            if root ~= nil then toast_root = root end
            if toast ~= nil and not fading then
                -- it went on after the toast: put the toast back on top, keeping its time
                local shown = toast_shown
                show(toast_text, toast_seconds)
                toast_shown = shown
                log_order("toast put back on top")
            end
        end
    end
    if toast ~= nil then
        local age = clock() - toast_shown
        if age >= toast_seconds + FADE then
            close_toast()
        elseif age >= toast_seconds and not fading and not pcall(fade_out) then
            close_toast()
        end
    end
    if toast == nil then
        local text, seconds = take_notice()
        if text ~= nil then
            local ok, err = pcall(show, text, tonumber(seconds))
            if ok then
                log("notices: showing " .. text .. " on " .. tostring(root_name(ui_root())))
                log_order("toast shown")
            else
                pcall(close_toast)
                log("notices error: " .. tostring(err))
            end
        end
    end
    check_pause_menu()
    local forced = take_pause()
    if forced ~= nil then try_pause(forced) end
end

local timer_root, errors = nil, 0

local function start_timer()
    local root = LUI.roots.UIRootFull or LUI.roots.UIRoot0
    if root == nil or root == timer_root then return end
    timer_root = root
    root:addElement(LUI.UITimer.newElementTimer(POLL_MS, false, function()
        local ok, err = pcall(poll)
        if not ok and errors < 5 then
            errors = errors + 1
            log("notices error: " .. tostring(err))
        end
    end))
    log("notices: ready on " .. tostring(root_name(root)))
end

-- notices.m's start chunk calls this for every menu made after this file has loaded
function bo3macfix.NoticesMenuCreated(name, menu)
    start_timer()
    new_menu, new_menu_name, new_menu_looks = menu, name, 0
    if name == "StartMenu_Main" then
        pause_menu, pause_menu_shown = menu, false
        LUI.OverrideFunction_CallOriginalSecond(menu, "close", function() pause_menu_closed(menu) end)
    end
end

start_timer()
