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
| `YieldSleep0` | 0 | Makes `Sleep(0)` yield like on Windows. |
| `FastSetjmp` | 1 | Makes the script VMs' `setjmp`/`longjmp` skip saving the signal mask, a syscall each time. |
| `FramesInFlight` | 2 | How many frames the CPU may queue ahead of the GPU, 1 to 3 (`MaxFrameLatency` in config.ini does nothing on Mac). |
| `FramePacingFPS` | 0 | Keeps each frame on screen for at least 1/N of a second, 30 to 240, for even pacing below the refresh rate. |
| `MetalFXUpscale` | 0 | Upscales with MetalFX when the game's resolution is lower than the screen's, in either fullscreen mode. 2 is for scaled monitor modes (like "looks like 2560x1440" on a 4K monitor): it upscales to the panel's own pixels rather than the bigger scaled size macOS shrinks back down. Needs macOS 13. |
| `DisplayFollow` | 1 | Follows display changes: when displays are plugged in or unplugged, the game moves to the display `GameDisplay` picks and adapts its resolution and refresh rate to it. |
| `GameDisplay` | 0 | Which display the game uses: 0 the main display (the one with the menu bar), 1 the MacBook's built-in screen while it's on, 2 an external display when one is connected (the last one the game used, else the fastest), 3 one display you picked by name on the in-game page (it's remembered in `BO3MacFix/displays.ini`; while it's not connected, the game goes by 2). |
| `DisplayMemory` | 1 | Remembers the resolution and refresh rate the game used on each display (in `BO3MacFix/displays.ini`), for when it's back on that display. A monitor is still recognised when it's plugged into another port or adapter. |
| `MatchRefreshRate` | 1 | Uses each display's own refresh rate. A `MaxFPS` that matched the old display's refresh rate follows it too. |
| `ConfineCursor` | 0 | Stops stray clicks landing on another display in fullscreen menus: the pointer stops at the edge of the game's display, and keeps going if you push on past it. |
| `ControllerMappings` | 1 | Lets DualShock 4, DualSense and DualSense Edge controllers work over USB. |
| `ControllerBridge` | 0 | Reads DualShock 4 and DualSense controllers through macOS instead: Bluetooth and USB, rumble and lightbar. Turn Steam Input off for the game. Needs macOS 11.3. |
| `ButtonPrompts` | 0 | 0 shows PlayStation buttons with a PlayStation controller, 1 always Xbox, 2 always PlayStation. |
| `LightbarColor` | -1 | The lightbar colour as RRGGBB, with `ControllerBridge`. -1 leaves it alone. |
| `GyroAim` | 0 | Experimental: aims with the DualSense's gyro on top of the right stick, 1 always, 2 while L2 is held. With `ControllerBridge`. `GyroSensitivity` (percent) and `GyroInvert` (1 vertical, 2 horizontal, 3 both) tune it. |
| `AdaptiveTriggers` | 0 | Experimental: gives the DualSense's R2 a weapon-style break and L2 light resistance. With `ControllerBridge`. |
| `PauseOnDisconnect` | 0 | Opens the pause menu when the controller you're playing with disconnects mid-match (a Bluetooth dropout, a flat battery), in games that can pause: solo and offline. With `ControllerBridge`. |
| `RawMouse` | 0 | Experimental: aims with raw mouse counts instead of macOS's accelerated movement. `RawMouseScale` (percent) sets the speed. |
| `AAEOnline` | 0 | Lets All-around Enhancement fetch its version notes and weather (HTTPS GET only, to raw.githubusercontent.com and wttr.in). |
| `ExitFix` | 1 | Quits without the crash (and crash report) Aspyr's shutdown causes every time. |
| `AudioFollowsDefault` | 1 | Moves the game's audio to the new default output device, e.g. when AirPods connect or disconnect. |
| `Notices` | 1 | Shows a short notice at the top of the screen when a controller connects or disconnects (with its battery level), when its battery gets low (20, 10 and 5 %), and when the game moves to another display. The controller ones need `ControllerBridge`. |
| `FrameStats` | 0 | Logs the frame rate, 1% low and worst frame time every N seconds. |
| `BatteryFPS` | 0 | Limits the frame rate to N (30 to 240) only while the Mac runs on battery, to save power. |
| `MetalHUD` | 0 | Shows macOS's Metal performance HUD (FPS and GPU time). |
| `GpuDiagnostics` | 0 | Logs the window, display mode and Metal layer whenever they change. |
| `NoCodePatches` | 0 | For testing only: leaves the game's code exactly as shipped, without any of the fixes. Solo play only. |

Most can also be set with an environment variable, `BO3MACFIX_` and the name in capitals. When the game is started
from Steam, BO3MacFix's log goes to `BO3MacFix/log.txt` and the launcher's to `BO3MacFix/launcher.log`.

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
