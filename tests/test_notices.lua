-- Runs source/notices.lua against a made-up LUI and Engine, with any Lua 5.x: lua tests/test_notices.lua
-- Checks the toast's life (shown, faded, closed, one at a time, put back on top when a menu is made) and when
-- PauseOnDisconnect pauses.
local failures = 0
local function check(condition, what)
    if not condition then
        failures = failures + 1
        print("FAIL: " .. what)
    end
end

local function element()
    local e = { children = {}, alpha = 1 }
    function e:setLeftRight() end
    function e:setTopBottom() end
    function e:setRGB() end
    function e:setAlignment() end
    function e:setAlpha(alpha) self.alpha = alpha end
    function e:setText(text) self.text = text end
    function e:beginAnimation(name, ms) self.animation = name end
    function e:addElement(child)
        table.insert(self.children, child)
        child.parent = self
    end
    function e:close()
        local siblings = self.parent and self.parent.children or {}
        for i, sibling in ipairs(siblings) do
            if sibling == self then table.remove(siblings, i) break end
        end
        self.closed = true
    end
    return e
end

local root = element()
local queue, pause_request, pauses, logs = {}, nil, 0, {}
local state = { in_game = true, can_pause = true, gamepad = true }
BO3MacFix = {
    NoticeLog = function(text) table.insert(logs, text) end,
    TakeNotice = function() return table.remove(queue, 1) end,
    TakePauseRequest = function() local r = pause_request pause_request = nil return r end,
    PauseGame = function() pauses = pauses + 1 end,
}
LUI = {
    roots = { UIRootFull = root },
    UIElement = { new = element }, UIImage = { new = element }, UIText = { new = element },
    UITimer = { newElementTimer = function(ms, once, fn) local t = element() t.timer, t.once = fn, once return t end },
    OverrideFunction_CallOriginalSecond = function(object, name, fn)
        local original = object[name]
        object[name] = function(...) original(...) fn(...) end
    end,
}
Enum = { LUIAlignment = { LUI_ALIGNMENT_CENTER = 1 } }
Engine = {
    IsInGame = function() return state.in_game end,
    CanPauseGame = function() return state.can_pause end,
    LastInput_Gamepad = function() return state.gamepad end,
}

local chunk = assert(loadfile(arg and arg[0] and arg[0]:gsub("test_notices%.lua$", "../source/notices.lua") or "source/notices.lua"))
chunk(false)

local timer = nil
for _, child in ipairs(root.children) do if child.timer then timer = child end end
check(timer ~= nil and timer.once == false, "a repeating timer on the root")
local function tick(times) for _ = 1, times or 1 do timer.timer() end end
local function toasts()
    local found = {}
    for _, child in ipairs(root.children) do
        if not child.timer then table.insert(found, child) end
    end
    return found
end
local function toast_text(toast)
    for _, child in ipairs(toast.children) do if child.text then return child.text end end
end

-- one notice: shown, faded after 4 s, gone after 4.5 s
table.insert(queue, "DualSense connected, battery 80%")
tick()
check(#toasts() == 1 and toast_text(toasts()[1]) == "DualSense connected, battery 80%", "the notice is shown")
tick(16) -- 4 s
check(#toasts() == 1 and toasts()[1].alpha == 0 and toasts()[1].animation == "fade_out", "it fades after 4 s")
tick(2)
check(#toasts() == 0, "and is gone after the fade")

-- two at once: one after the other
table.insert(queue, "first")
table.insert(queue, "second")
tick()
check(#toasts() == 1 and toast_text(toasts()[1]) == "first", "the first of two")
tick(18)
check(#toasts() == 1 and toast_text(toasts()[1]) == "second", "then the second")
tick(18)
check(#toasts() == 0, "then none")

-- a menu made while a toast shows: the toast goes back on top, keeping its time
table.insert(queue, "Game moved to XB273K GP, 120 Hz")
tick(4)
local menu = element()
root:addElement(menu)
BO3MacFix.NoticesMenuCreated("SomeMenu", menu)
tick()
local last = root.children[#root.children]
check(not last.timer and toast_text(last) == "Game moved to XB273K GP, 120 Hz", "the toast is back on top of the menu")
check(#toasts() == 2, "one toast and the menu") -- the menu counts as a non-timer child here
tick(13)
check(toast_text(root.children[#root.children]) ~= nil and root.children[#root.children].alpha == 0, "it kept its time")
tick(2)
menu:close()

-- pausing
pause_request = false
tick()
check(pauses == 1, "pauses in a match that can pause, last input a controller")
pause_request = false
state.gamepad = false
tick()
check(pauses == 1, "not when the last input was the keyboard or mouse")
pause_request = true -- the test hook: whatever the last input was
tick()
check(pauses == 2, "the test hook pauses anyway")
state.gamepad = true
state.can_pause = false
pause_request = false
tick()
check(pauses == 2, "not in a game that can't pause")
state.can_pause = true
state.in_game = false
pause_request = false
tick()
check(pauses == 2, "not outside a match")
state.in_game = true
local start_menu = element()
root:addElement(start_menu)
BO3MacFix.NoticesMenuCreated("StartMenu_Main", start_menu)
pause_request = false
tick()
check(pauses == 2, "not while the pause menu is open")
start_menu:close()
pause_request = false
tick()
check(pauses == 3, "again once it's closed")

-- a broken Engine function doesn't stop it
Engine.CanPauseGame = function() error("boom") end
pause_request = false
tick()
check(pauses == 4, "an Engine function that fails counts as unknown")

if failures == 0 then print("test_notices.lua: all passed") end
os.exit(failures == 0 and 0 or 1)
