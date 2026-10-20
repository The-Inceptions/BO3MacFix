# BO3MacFix

Fixes Black Ops 3 on Mac so custom maps from the Steam Workshop can be played and online matches can be played
with people running the Windows version of the game.

by Emma / InvoxiPlayGames and Anthony / serious.

### Any issues you run into with custom maps or workshop mods should be reported here, and *not* to the mod's author!

## Install Guide: [Installing BO3MacFix](https://github.com/InvoxiPlayGames/BO3MacFix/wiki/Installing-BO3MacFix)

## FAQ: [Frequently Asked Questions](https://github.com/InvoxiPlayGames/BO3MacFix/wiki/Frequently-Asked-Questions-(FAQ))

## Settings

BO3MacFix reads `BO3MacFix/settings.ini` in the game's folder when the game starts, one `Name = value` per line
(lines starting with `//` are comments). All of them are optional.

They can also be changed in the game: pick **BO3MACFIX SETTINGS** in the main menu, or press F10 in the start
menu. Settings marked "(restart)" take effect after a restart, which the page offers to do. The Display ones apply
right away, mid-match too: changing **Game display** moves the game to that display.

| Setting | Default | What it does |
| --- | --- | --- |
| `PrePatchedExe` | 1 | Starts a copy of the game with BO3MacFix's changes already made in the file (`CoDBlkOps3_Exe.bo3macfix`, remade after game or BO3MacFix updates), so Rosetta runs it from its ahead-of-time translation instead of its slower JIT. 0 starts the original. |
| `JobThreads` | 2 | How many job worker threads the game uses, 2 to 4. |
| `ThreadQoS` | 1 | Runs the game's main, render and job threads at the highest scheduling priority. |
| `JobIdleFix` | 1 | Lets idle job threads sleep instead of spinning. |
| `RenderIdleFix` | 1 | Lets the render thread sleep while it waits for the next frame, instead of spinning. |
| `MainWaitFix` | 1 | Lets the main thread sleep while it waits for the render thread, instead of spinning. |
| `YieldSleep0` | 0 | Makes `Sleep(0)` yield like on Windows. |
| `FastSetjmp` | 1 | Makes the script VMs' `setjmp`/`longjmp` skip saving the signal mask, a syscall each time. |
| `FastScriptErrors` | 1 | Stops each script error from trying to open the script source files (`scriptsrc.zip`, only the mod tools make them) again for every line of its call stack. All-around Enhancement's scripts hit thousands of errors a second, so this frees up much of the main thread. |
| `QuietScriptErrors` | 1 | Shows each repeated script error (the same message) in `console_mp.log` 3 times with its call stack, then only counts it: once a minute a line says how many more there were. Mod scripts that hit an error every frame otherwise write that log at up to megabytes a second. 0 prints every error, as before. |
| `FramesInFlight` | 2 | How many frames the CPU may queue ahead of the GPU, 1 to 3 (`MaxFrameLatency` in config.ini does nothing on Mac). |
| `FramePacingFPS` | 0 | Keeps each frame on screen for at least 1/N of a second, 30 to 240, for even pacing below the refresh rate. |
| `MetalFXUpscale` | 0 | Upscales with MetalFX when the game's resolution is lower than the screen's, in either fullscreen mode. 2 is for scaled monitor modes (like "looks like 2560x1440" on a 4K monitor): it upscales to the panel's own pixels rather than the bigger scaled size macOS shrinks back down. Needs macOS 13. |
| `DynamicResolution` | 0 | Lowers the 3D scene's resolution (`ResolutionPercent` in config.ini) in 5% steps while the GPU can't keep up with the display's refresh rate (or a lower frame limit), down to this share of it (50 to 95, e.g. 75), and raises it again once there's room. The HUD stays at full resolution. config.ini keeps your own value. 0 = off. |
| `DisplayFollow` | 1 | Follows display changes: when displays are plugged in or unplugged, the game moves to the display `GameDisplay` picks and adapts its resolution and refresh rate to it. |
| `GameDisplay` | 0 | Which display the game uses: 0 the main display (the one with the menu bar), 1 the MacBook's built-in screen while it's on, 2 an external display when one is connected (the last one the game used, else the fastest), 3 one display you picked by name on the in-game page (it's remembered in `BO3MacFix/displays.ini`; while it's not connected, the game goes by 2). |
| `DisplayMemory` | 1 | Remembers the resolution and refresh rate the game used on each display (in `BO3MacFix/displays.ini`), for when it's back on that display. A monitor is still recognised when it's plugged into another port or adapter. |
| `MatchRefreshRate` | 1 | Uses each display's own refresh rate. A `MaxFPS` that matched the old display's refresh rate follows it too. |
| `ConfineCursor` | 0 | Stops stray clicks landing on another display in fullscreen menus: the pointer stops at the edge of the game's display, and keeps going if you push on past it. |
| `AvoidNotch` | 0 | In fullscreen on a MacBook's built-in screen, keeps the whole picture below the camera notch: the strip beside the notch goes black, and the picture gets a little smaller to fit, with thin black bars at the sides. The mouse follows it. It turns itself off on displays without a notch and on again when the game comes back. |
| `ControllerMappings` | 1 | Lets DualShock 4, DualSense and DualSense Edge controllers work over USB. |
| `ControllerBridge` | 0 | Reads DualShock 4 and DualSense controllers through macOS instead: Bluetooth and USB, rumble and lightbar. Turn Steam Input off for the game. Needs macOS 11.3. |
| `ButtonPrompts` | 0 | 0 shows PlayStation buttons with a PlayStation controller, 1 always Xbox, 2 always PlayStation. |
| `LightbarColor` | -1 | The lightbar colour as RRGGBB, with `ControllerBridge`. -1 leaves it alone. |
| `GyroAim` | 0 | Experimental: aims with the DualSense's gyro on top of the right stick, 1 always, 2 while L2 is held. With `ControllerBridge`. `GyroSensitivity` (percent) and `GyroInvert` (1 vertical, 2 horizontal, 3 both) tune it. |
| `AdaptiveTriggers` | 0 | Experimental: gives the DualSense's R2 a weapon-style break and L2 light resistance. With `ControllerBridge`. |
| `PauseOnDisconnect` | 0 | Opens the pause menu when the controller you're playing with disconnects mid-match (a Bluetooth dropout, a flat battery), in games that can pause: solo and offline. With `ControllerBridge`. |
| `RawMouse` | 0 | Experimental: aims with raw mouse counts instead of macOS's accelerated movement. `RawMouseScale` (percent) sets the speed. |
| `AAEOnline` | 0 | Lets All-around Enhancement fetch its version notes and weather (HTTPS GET only, to raw.githubusercontent.com and wttr.in). |
| `AAEBankFix` | 1 | Keeps cheats (`sv_cheats`) off as a map starts with All-around Enhancement loaded, so its bank, weapon locker and saved bank balance load. The game turns cheats on for any map started with a mod, and AAE's bank checks before AAE turns them off again. `devmap`, AAE's own `elmg_cheats` and other mods keep the game's behaviour. Applies after a restart. |
| `ExitFix` | 1 | Quits without the crash (and crash report) Aspyr's shutdown causes every time. |
| `AudioFollowsDefault` | 1 | Moves the game's audio to the new default output device, e.g. when AirPods connect or disconnect. |
| `Notices` | 1 | Shows a short notice at the top of the screen when a controller connects or disconnects (with its battery level), when its battery gets low (20, 10 and 5 %), and when the game moves to another display. The controller ones need `ControllerBridge`. |
| `AutoSave` | 1 | Saves a solo zombies game by itself each time a new round starts, to the same `BO3MacFix/checkpoint.ini` as the settings page's Save game, so a crash or a force quit loses at most the round being played. A game saved at a higher round stays unless this game resumed it, and a notice says "Saved round N". The file is written on a background thread, at most every 15 seconds. The page's Delete saved game removes the save. |
| `QuickSave` | 96 | A key that saves a solo zombies game like the settings page's Save game, with the same checks: the macOS key code of F5 (96), F6 (97), F7 (98), F8 (100) or F9 (101), or 0 for off. Holding L3 + R3 for a second in the pause menu does the same, with `ControllerBridge`. A notice says "Game saved: round N" or why not. A game saved at a higher round that this one didn't resume takes a second press within 5 seconds; with `Notices` off (the question can't be shown) the key doesn't replace it, the settings page's Save game row does. |
| `FrameStats` | 0 | Logs the frame rate, 1% low and worst frame time every N seconds. |
| `BatteryFPS` | 0 | Limits the frame rate to N (30 to 240) only while the Mac runs on battery, to save power. |
| `PauseMenuFPS` | 30 | Limits the frame rate to N (30 to 240) while the pause menu is open in a match, to save power, and lifts the limit as soon as it closes. 0 turns it off. The settings page offers Off, 30, 40 and 60, rates that divide a 120 Hz display's refresh evenly (45 came out at 40 there). With `FramePacingFPS` or `BatteryFPS` on too, the lowest limit wins. |
| `MenuFPS` | 40 | Limits the frame rate to N (30 to 240) in the menus outside a match (the main menu, the lobby, the mods menu), to save power: the stock main menu otherwise renders at the display's 120 Hz. It ends as soon as a match starts, and as a map starts loading. 0 turns it off. The settings page offers Off, 30, 40 and 60. With other limits on too, the lowest that applies wins. |
| `BackgroundFPS` | 30 | Limits the frame rate to N (10 to 240) while the game isn't the active app, or is hidden or minimized, to save power (the game itself keeps rendering at full speed behind other apps), and lifts it as soon as the game is in front again. It only applies with the menus or a match up, not while the game starts or a map loads. 0 turns it off. The settings page offers Off, 15, 30 and 60. |
| `MetalHUD` | 0 | Shows macOS's Metal performance HUD (FPS and GPU time). |
| `GpuDiagnostics` | 0 | Logs the window, display mode and Metal layer whenever they change. |
| `NoCodePatches` | 0 | For testing only: leaves the game's code exactly as shipped, without any of the fixes. Solo play only. |
| `ModWarningDialog` | 0 | When a mod tries to load a Windows DLL, BO3MacFix warns once a launch with a notice (and in the log). 1 shows the old "BO3MacFix Mod Warning" dialog instead, which stops the game until it's closed, fullscreen too. |

Most can also be set with an environment variable, `BO3MACFIX_` and the name in capitals. When the game is started
from Steam, BO3MacFix's log goes to `BO3MacFix/log.txt` and the launcher's to `BO3MacFix/launcher.log`.

All-around Enhancement only runs on the latest Windows build: it compares the game's changelist and build time with
that build's, and on anything else opens its downgrade guide and quits. BO3MacFix gives it the values it expects,
read from AAE's own `core_mod.ff` when the game starts (once per AAE update, then remembered in
`BO3MacFix/aae_build.cache`), so an AAE update for a new Windows patch doesn't need a new BO3MacFix. `log.txt` says
what it found, in lines starting with `AAE build check:`. If AAE opens its downgrade guide anyway, two settings that
aren't on the in-game page set the values by hand:

| Setting | Example | What it does |
| --- | --- | --- |
| `AAEChangelist` | `13892626` | The changelist AAE checks for. |
| `AAEBuildTime` | `Wed Dec 10 17:10:46 2025` | The build time AAE checks for, the rest of the line (quotes are optional). |

## Credits

- [Alyssa / alltheamps](https://github.com/shiversoftdev) - guidance, help, t7patch and the cod2map64 patch DLL.
- [The Dobby project](https://github.com/jmpews/Dobby) - code hooking on macOS. (used under the MIT license)
- [Mickael DE LA ROQUE / mickadlr](https://github.com/mickadlr/BO3MacFix) - the custom map stream-stall workaround.

## What does it do? (Nerd stuff)

### Custom Maps

There are two big issues with Steam Workshop map loading in the current (v98, from 2019) Mac build of BO3:

* Texture (.png) conversion crashes the game when hovering or selecting a workshop map
* Havok navmesh/navvolume files are a different format across PC and Mac

This project hooks into the game to solve both these problems. It solves the texture problem by stubbing out
the faulty function, and solves the navmesh problem by converting it on-the-fly using a modded version of the
modding SDK. We call Wine from within the native Mac game process to call upon the modding SDK, after saving
a snapshot of the Havok navmesh/navvolume data from the map, and the patch to the SDK's cod2map tool takes
that input and outputs in the Mac-encoded format.

A campaign mission with broken navigation meshes is also fixed in the same way.

### Online Play

A macOS update updated the version of libcurl that was included with the OS, which makes online login fail
in Black Ops 3 due to improper usage of the cURL library that's technically "undefined behaviour". This
is fixed here by removing one of the HTTP headers the game manually sets and letting cURL do its thing.

Version numbers are spoofed when signing in to match the Windows version's to enable matchmaking between the
two, along with using updated scripts and assets from the Windows version.
