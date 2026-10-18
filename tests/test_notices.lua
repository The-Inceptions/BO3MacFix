-- Runs source/notices.lua against a made-up LUI and Engine, with any Lua 5.x: lua tests/test_notices.lua
-- Checks the toast's life (shown, faded, closed, one at a time), that it goes on the root of the latest menu and back
-- on top when a menu is made, and when PauseOnDisconnect pauses.
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
    function e:getParent() return self.parent end
    function e:getPriority() return 0 end
    function e:addElement(child)
        table.insert(self.children, child)
        child.parent = self
    end
    function e:close()
        local siblings = self.parent and self.parent.children or {}
        for i, sibling in ipairs(siblings) do
            if sibling == self then table.remove(siblings, i) break end
        end
        self.parent = nil
        self.closed = true
    end
    return e
end

local full, root0 = element(), element()
local now = 0
local queue, pause_request, pauses, logs = {}, nil, 0, {}
local state = { in_game = true, can_pause = true, gamepad = true }
BO3MacFix = {
    NoticeLog = function(text) table.insert(logs, text) end,
    NoticeClock = function() return now end,
    TakeNotice = function() return table.remove(queue, 1) end,
    TakePauseRequest = function() local r = pause_request pause_request = nil return r end,
    PauseGame = function() pauses = pauses + 1 end,
}
LUI = {
    roots = { UIRootFull = full, UIRoot0 = root0 },
    UIElement = { new = element }, UIImage = { new = element }, UIText = { new = element },
    UITimer = { newElementTimer = function(ms, once, fn) local t = element() t.timer, t.once, t.ms = fn, once, ms return t end },
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

local here = arg and arg[0] and arg[0]:match("^(.*)/[^/]*$") or "tests"
local chunk = assert(loadfile(here .. "/../source/notices.lua"))
chunk(false)

local timer = nil
for _, child in ipairs(full.children) do if child.timer then timer = child end end
check(timer ~= nil and timer.once == false and timer.ms == 250, "a repeating timer on UIRootFull")
-- each tick is a quarter of a second
local function tick(times)
    for _ = 1, times or 1 do
        now = now + 0.25
        timer.timer()
    end
end
local function toast_on(root)
    for i = #root.children, 1, -1 do
        local child = root.children[i]
        for _, part in ipairs(child.children) do
            if part.text then return child, part.text, i == #root.children end
        end
    end
end

-- one notice: shown, faded after 4 s, gone after 4.5 s
table.insert(queue, "DualSense connected, battery 80%")
tick()
local toast, text = toast_on(full)
check(text == "DualSense connected, battery 80%", "the notice is shown on UIRootFull before any menu")
tick(15)
check(toast.alpha == 1, "still fully shown just before 4 s")
tick(1)
check(toast.alpha == 0 and toast.animation == "fade_out", "it fades at 4 s")
tick(2)
check(toast_on(full) == nil, "and is gone after the fade")

-- two at once: one after the other
table.insert(queue, "first")
table.insert(queue, "second")
tick()
check(select(2, toast_on(full)) == "first", "the first of two")
tick(18)
check(select(2, toast_on(full)) == "second", "then the second")
tick(18)
check(toast_on(full) == nil, "then none")

-- a slow timer doesn't make toasts last longer: it goes by the clock
table.insert(queue, "slow")
tick()
now = now + 4.5
timer.timer()
check(toast_on(full) == nil, "gone after 4.5 s of clock, however few ticks")

-- in a match the HUD goes on UIRoot0: toasts go there, after it
local hud = element()
BO3MacFix.NoticesMenuCreated("T7Hud_zm_dlc5", hud)
root0:addElement(hud)
tick()
table.insert(queue, "Test notice 1")
tick()
local _, on_root0, last = toast_on(root0)
check(on_root0 == "Test notice 1" and last, "the toast goes on the HUD's root, after the HUD")
check(toast_on(full) == nil, "not on UIRootFull")

-- the pause menu is made and goes on after the toast: the toast goes back on top, keeping its time
tick(4)
local pause_menu = element()
BO3MacFix.NoticesMenuCreated("StartMenu_Main", pause_menu)
root0:addElement(pause_menu)
tick()
local _, raised, on_top = toast_on(root0)
check(raised == "Test notice 1" and on_top, "the toast is back on top of the pause menu")
tick(10)
check(select(1, toast_on(root0)).alpha == 1, "it kept its time: not fading 3.75 s after it was first shown")
tick(1)
check(select(1, toast_on(root0)).alpha == 0, "and fading at 4 s")
tick(2)
check(toast_on(root0) == nil, "then it's gone")

-- pausing: not with the pause menu open
pause_request = false
tick()
check(pauses == 0, "not while the pause menu is open")
pause_menu:close()
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

-- a broken Engine function doesn't stop it
Engine.CanPauseGame = function() error("boom") end
pause_request = false
tick()
check(pauses == 3, "an Engine function that fails counts as unknown")
-- numbers for booleans: 0 is false
Engine.CanPauseGame = function() return 1 end
Engine.LastInput_Gamepad = function() return 0 end
pause_request = false
tick()
check(pauses == 3, "a 0 from LastInput_Gamepad means the keyboard or mouse")
Engine.LastInput_Gamepad = function() return 1 end
pause_request = false
tick()
check(pauses == 4, "and a 1 a controller")

-- a menu that never goes on a root doesn't move the toasts
local stray = element()
BO3MacFix.NoticesMenuCreated("Stray", stray)
tick(5)
table.insert(queue, "after a stray menu")
tick()
check(select(2, toast_on(root0)) == "after a stray menu", "toasts stay on the last root a menu went on")

if failures == 0 then print("test_notices.lua: all passed") end
os.exit(failures == 0 and 0 or 1)
