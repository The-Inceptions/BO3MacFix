// BO3MacFix's settings: every settings.ini option with what an in-game menu needs to show it, a writer that changes
// one line of settings.ini, restarting the game, and the BO3MacFix Lua table:
//   BO3MacFix.GetSettings() -> { { name, label, description, category, kind = "toggle"|"choice", values, labels,
//                                  value, default, live }, ... }
//   BO3MacFix.SetSetting(name, value) -> restart_needed, or nil if it couldn't be saved
//   BO3MacFix.RestartGame()
//   BO3MacFix.RestartPending() -> true while a setting saved this session differs from what the game started with
//                                  and couldn't be applied without a restart
// Some settings apply right away (live = true): the frame rate ones, the display ones, the controller ones,
// RawMouseScale, AAEOnline, ExitFix, AutoSave, QuickSave and ModWarningDialog, plus JobIdleFix, RenderIdleFix, MainWaitFix and AvoidNotch from the pre-patched executable. The rest
// are read once at startup, so changing them asks for a restart.
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "checkpoint_autosave.h"
#include "controllers.h"
#include "cpu_tuning.h"
#include "display_follow.h"
#include "gc_bridge.h"
#include "gpu_tweaks.h"
#include "notch.h"
#include "notices.h"
#include "offsets.h"
#include "prepatch.h"
#include "qol.h"
#include "quick_save.h"
#include "rawmouse.h"
#include "settings.h"
#include "settings_ini.h"
#include "t7overcharged.h"

#define Settings_File "../../../BO3MacFix/settings.ini"
#define Max_Choices 8

extern char **environ;
extern uint64_t game_base_address;

// when a change applies without a restart
enum {
    Live_Never,     // read once at startup
    Live_Always,
    Live_Present,   // needs Aspyr's present hooked: already, or late from the pre-patched executable
    Live_Prepatch,  // a value slot of the pre-patched executable
    Live_Prompts,   // needs the prompts hook: already, or late from the pre-patched executable
    Live_Notch,     // needs AvoidNotch's hooks: already, or late from the pre-patched executable
};

typedef struct {
    const char *name, *label, *description, *category;
    bool hex;                       // written as RRGGBB
    int count;                      // 0 = toggle (0/1)
    int values[Max_Choices];
    const char *labels[Max_Choices];
    int fallback;                   // the default
    int live;                       // Live_*
} setting_t;

#define Toggle(def) false, 0, { 0 }, { 0 }, def, Live_Never
#define Toggle_Live(def, live) false, 0, { 0 }, { 0 }, def, live
static const setting_t settings[] = {
    { "PrePatchedExe", "Pre-patched executable", "Starts a copy of the game with BO3MacFix's changes in the file, so Rosetta runs it from its faster translation.", "Performance", Toggle(1) },
    { "JobThreads", "Job threads", "How many worker threads the game's job system uses; 4 measured about 8% faster.", "Performance", false, 3, { 2, 3, 4 }, { "2", "3", "4" }, 2, false },
    { "ThreadQoS", "Thread priority", "Runs the game's main, render and job threads at macOS's highest scheduling priority.", "Performance", Toggle(1) },
    { "JobIdleFix", "Idle job threads sleep", "Lets idle job threads sleep instead of spinning while they wait for work.", "Performance", Toggle_Live(1, Live_Prepatch) },
    { "RenderIdleFix", "Idle render thread sleeps", "Lets the render thread sleep while it waits for the next frame instead of spinning.", "Performance", Toggle_Live(1, Live_Prepatch) },
    { "MainWaitFix", "Main thread sleeps", "Lets the main thread sleep while it waits for the render thread instead of spinning. Saves power, most of all in the menus and the pause menu.", "Performance", Toggle_Live(1, Live_Prepatch) },
    { "FastSetjmp", "Fast script calls", "Makes the script engines skip a slow system call they make constantly.", "Performance", Toggle(1) },
    { "FastScriptErrors", "Fast script errors", "Stops script errors from looking for missing script source files every time. Mods with many errors run faster.", "Performance", Toggle(1) },
    { "QuietScriptErrors", "Quiet script errors", "Shows each repeated script error in console_mp.log 3 times, then only counts it.", "Performance", Toggle(1) },
    { "YieldSleep0", "Yielding Sleep(0)", "Makes Sleep(0) yield like on Windows: a little more FPS for more CPU.", "Performance", Toggle(0) },
    { "FramesInFlight", "Frames in flight", "How many frames the CPU may queue ahead of the GPU: fewer means less input lag.", "Graphics", false, 3, { 1, 2, 3 }, { "1 (least lag)", "2 (normal)", "3 (smoothest)" }, 2, false },
    { "FramePacingFPS", "Frame pacing", "Keeps each frame on screen for a minimum time, for even pacing below the refresh rate.", "Graphics", false, 6, { 0, 30, 45, 60, 90, 120 }, { "Off", "30", "45", "60", "90", "120" }, 0, Live_Present },
    { "BatteryFPS", "Battery frame limit", "Limits the frame rate only while the Mac runs on battery, to save power.", "Graphics", false, 5, { 0, 30, 45, 60, 90 }, { "Off", "30", "45", "60", "90" }, 0, Live_Present },
    { "PauseMenuFPS", "Pause menu frame limit", "Limits the frame rate while the pause menu is open in a match, to save power.", "Graphics", false, 4, { 0, 30, 40, 60 }, { "Off", "30", "40", "60" }, 30, Live_Present },
    { "MenuFPS", "Menu frame limit", "Limits the frame rate in the menus outside a match (main menu, lobby, mods), to save power.", "Graphics", false, 4, { 0, 30, 40, 60 }, { "Off", "30", "40", "60" }, 40, Live_Present },
    { "BackgroundFPS", "Background frame limit", "Limits the frame rate while the game isn't the active app, or is hidden or minimized, to save power.", "Graphics", false, 4, { 0, 15, 30, 60 }, { "Off", "15", "30", "60" }, 30, Live_Present },
    { "DynamicResolution", "Dynamic resolution", "Lowers the 3D scene's resolution a little while the GPU can't keep up with the refresh rate (down to this share of Render Resolution), and raises it again when it can.", "Graphics", false, 4, { 0, 85, 75, 70 }, { "Off", "Down to 85%", "Down to 75%", "Down to 70%" }, 0, Live_Present },
    { "MetalFXUpscale", "MetalFX upscaling", "Upscales with MetalFX when the game runs below the screen's resolution. \"Panel resolution\" is for scaled monitor modes.", "Graphics", false, 3, { 0, 1, 2 }, { "Off", "On", "On, panel resolution" }, 0, Live_Never },
    { "MetalHUD", "Metal performance HUD", "Shows macOS's Metal HUD with the frame rate and GPU time.", "Graphics", Toggle(0) },
    { "DisplayFollow", "Follow display changes", "Moves the game when displays are plugged in or unplugged, and adapts its resolution and refresh rate to the new display.", "Display", Toggle_Live(1, Live_Always) },
    { "GameDisplay", "Game display", "Which display the game uses: the main display (with the menu bar), the MacBook's screen, any external monitor, or one you pick by name.", "Display", false, 3, { 0, 1, 2 }, { "Main display", "Built-in", "External" }, 0, Live_Always },
    { "DisplayMemory", "Remember each display", "Remembers the resolution and refresh rate the game used on each display.", "Display", Toggle_Live(1, Live_Always) },
    { "MatchRefreshRate", "Match refresh rate", "Uses each display's own refresh rate, like 240 Hz on a gaming monitor and 120 Hz on the MacBook.", "Display", Toggle_Live(1, Live_Always) },
    { "ConfineCursor", "Keep cursor on game display", "Stops stray clicks landing on another display in fullscreen menus; push past the edge to leave.", "Display", Toggle_Live(0, Live_Always) },
    { "AvoidNotch", "Keep picture below notch", "In fullscreen on the MacBook's screen, shows the whole picture below the camera notch; the strip beside it goes black.", "Display", Toggle_Live(0, Live_Notch) },
    { "ControllerMappings", "PlayStation controllers over USB", "Lets DualShock 4, DualSense and DualSense Edge controllers work over USB.", "Controller", Toggle(1) },
    { "ControllerBridge", "macOS controller support", "Reads PlayStation controllers through macOS: Bluetooth and USB, rumble and lightbar. Turn Steam Input off.", "Controller", Toggle(0) },
    { "ButtonPrompts", "Button prompts", "Which button icons the game shows.", "Controller", false, 3, { 0, 1, 2 }, { "Automatic", "Xbox", "PlayStation" }, 0, Live_Prompts },
    { "LightbarColor", "Lightbar colour", "The DualSense or DualShock 4 lightbar colour (with macOS controller support).", "Controller", true, 7, { -1, 0xFF0000, 0x0040FF, 0x00C000, 0xFF00FF, 0xFFA000, 0xFFFFFF }, { "Leave alone", "Red", "Blue", "Green", "Pink", "Orange", "White" }, -1, Live_Always },
    { "GyroAim", "Gyro aiming", "Aims with the DualSense's gyro on top of the right stick (with macOS controller support).", "Controller", false, 3, { 0, 1, 2 }, { "Off", "Always", "While aiming (L2)" }, 0, Live_Always },
    { "GyroSensitivity", "Gyro sensitivity", "How strongly the gyro turns the view, in percent.", "Controller", false, 7, { 25, 50, 75, 100, 150, 200, 300 }, { "25%", "50%", "75%", "100%", "150%", "200%", "300%" }, 100, Live_Always },
    { "GyroInvert", "Gyro invert", "Inverts the gyro's vertical and/or horizontal direction.", "Controller", false, 4, { 0, 1, 2, 3 }, { "Off", "Vertical", "Horizontal", "Both" }, 0, Live_Always },
    { "AdaptiveTriggers", "Adaptive triggers", "Gives the DualSense's R2 a weapon-style break and L2 light resistance.", "Controller", Toggle_Live(0, Live_Always) },
    { "PauseOnDisconnect", "Pause when the controller disconnects", "Opens the pause menu when your controller disconnects mid-match, in games that can pause (solo, offline). With macOS controller support.", "Controller", Toggle_Live(0, Live_Always) },
    { "RawMouse", "Raw mouse input", "Aims with raw mouse movement instead of macOS's accelerated movement.", "Mouse", Toggle(0) },
    { "RawMouseScale", "Raw mouse speed", "The speed of raw mouse aiming, in percent.", "Mouse", false, 7, { 25, 50, 75, 100, 150, 200, 300 }, { "25%", "50%", "75%", "100%", "150%", "200%", "300%" }, 100, Live_Always },
    { "ExitFix", "Quit without crashing", "Quits without the crash (and crash report) every quit used to end in.", "Game", Toggle_Live(1, Live_Always) },
    { "SkipIntro", "Skip intro movies", "Starts without the Black Ops III and Aspyr logo movies, so the title screen comes up about 20 seconds sooner.", "Game", Toggle(0) },
    { "AudioFollowsDefault", "Audio follows output device", "Moves the game's sound to the new output device, for example when AirPods connect.", "Game", Toggle(0) },
    { "Notices", "On-screen notices", "Shows a short notice when a controller connects or disconnects or its battery runs low, and when the game moves to another display.", "Game", Toggle_Live(1, Live_Always) },
    { "AAEOnline", "AAE online notes", "Lets All-around Enhancement show its version notes and weather (GitHub and wttr.in only).", "Game", Toggle_Live(0, Live_Always) },
    { "AAEBankFix", "AAE bank and locker", "Keeps cheats off as a map starts with All-around Enhancement (not with devmap or its own elmg_cheats), so its bank and weapon locker load.", "Game", Toggle(0) },
    { "FrameStats", "Frame stats in the log", "Logs the frame rate, 1% low and worst frame time to BO3MacFix/log.txt.", "Advanced", false, 4, { 0, 5, 10, 30 }, { "Off", "Every 5 s", "Every 10 s", "Every 30 s" }, 0, Live_Present },
    { "GpuDiagnostics", "GPU diagnostics in the log", "Logs the window, display mode and Metal layer whenever they change.", "Advanced", Toggle(0) },
    { "NoCodePatches", "No code patches (testing)", "Leaves the game's code exactly as shipped, without any of the fixes. Solo play only.", "Advanced", Toggle(0) },
    { "ModWarningDialog", "Mod warning as a dialog", "Warns about a mod's Windows DLL in a dialog that stops the game until it's closed, instead of an on-screen notice.", "Advanced", Toggle_Live(0, Live_Always) },
    // last, so the settings page shows them with the saved game's rows (checkpoint.lua)
    { "QuickSave", "Quick save key", "Saves a solo zombies game like Save game: this key, or L3 + R3 held for a second in the pause menu. A game saved at a higher round takes two presses, and isn't replaced this way with Notices off.", "Saved game", false, 6, { 0, 96, 97, 98, 100, 101 }, { "Off", "F5", "F6", "F7", "F8", "F9" }, 96, Live_Always },
    { "AutoSave", "AutoSave", "Saves a solo zombies game by itself as each new round starts, like Save game. A game saved at a higher round stays unless this one resumed it.", "Saved game", Toggle_Live(1, Live_Always) },
};
#define Setting_Count (sizeof(settings) / sizeof(settings[0]))

// each setting's value in settings.ini when the game started, and whether the value saved since needs a restart
static int startup_values[Setting_Count];
static bool startup_known = false;
static bool needs_restart[Setting_Count];

// whether changing this setting now takes effect without a restart
static bool live_now(const setting_t *t) {
    switch (t->live) {
    case Live_Always:
        return true;
    case Live_Present:
        return gpu_tweaks_live_possible();
    case Live_Prepatch:
        return prepatch_active();
    case Live_Prompts:
        return controllers_prompts_live();
    case Live_Notch:
        return notch_live_possible();
    default:
        return false;
    }
}

// applies a live setting's new value, false if it couldn't be (then it needs a restart)
static bool apply_live(const setting_t *t, int value) {
    const char *n = t->name;
    if (display_follow_owns(n))
        return display_follow_set_option(n, value) != 0;
    if (strcmp(n, "FramePacingFPS") == 0)
        return gpu_tweaks_set_pacing(value);
    if (strcmp(n, "BatteryFPS") == 0)
        return gpu_tweaks_set_battery_fps(value);
    if (strcmp(n, "PauseMenuFPS") == 0)
        return gpu_tweaks_set_pause_menu_fps(value);
    if (strcmp(n, "MenuFPS") == 0)
        return gpu_tweaks_set_menu_fps(value);
    if (strcmp(n, "BackgroundFPS") == 0)
        return gpu_tweaks_set_background_fps(value);
    if (strcmp(n, "FrameStats") == 0)
        return gpu_tweaks_set_frame_stats(value);
    if (strcmp(n, "DynamicResolution") == 0)
        return gpu_tweaks_set_dynamic_resolution(value);
    if (strcmp(n, "JobIdleFix") == 0)
        return cpu_tuning_set_wait_fix(Wait_Fix_Job, value);
    if (strcmp(n, "RenderIdleFix") == 0)
        return cpu_tuning_set_wait_fix(Wait_Fix_Render, value);
    if (strcmp(n, "MainWaitFix") == 0)
        return cpu_tuning_set_wait_fix(Wait_Fix_Main, value);
    if (strcmp(n, "ButtonPrompts") == 0)
        return controllers_set_button_prompts(value);
    if (strcmp(n, "AvoidNotch") == 0)
        return notch_set_enabled(value);
    if (strcmp(n, "GyroAim") == 0)
        gc_bridge_set_gyro_mode(value);
    else if (strcmp(n, "GyroSensitivity") == 0)
        gc_bridge_set_gyro_sensitivity(value);
    else if (strcmp(n, "GyroInvert") == 0)
        gc_bridge_set_gyro_invert(value);
    else if (strcmp(n, "AdaptiveTriggers") == 0)
        gc_bridge_set_adaptive_triggers(value);
    else if (strcmp(n, "LightbarColor") == 0)
        gc_bridge_set_lightbar(value);
    else if (strcmp(n, "RawMouseScale") == 0)
        rawmouse_set_scale(value);
    else if (strcmp(n, "AAEOnline") == 0)
        t7o_set_online(value);
    else if (strcmp(n, "ExitFix") == 0)
        qol_set_exit_fix(value);
    else if (strcmp(n, "Notices") == 0)
        notices_set_enabled(value);
    else if (strcmp(n, "PauseOnDisconnect") == 0)
        notices_set_pause_on_disconnect(value);
    else if (strcmp(n, "AutoSave") == 0)
        checkpoint_autosave_set_enabled(value != 0);
    else if (strcmp(n, "QuickSave") == 0)
        quick_save_set_key(value);
    else if (strcmp(n, "ModWarningDialog") == 0)
        ; // read when a mod's DLL doesn't load (bo3macfix.c)
    else
        return false;
    return true;
}

static const setting_t *find_setting(const char *name) {
    for (size_t i = 0; i < Setting_Count; i++) {
        if (strcmp(settings[i].name, name) == 0)
            return &settings[i];
    }
    return NULL;
}

// the value of Name in settings.ini, or fallback (read like read_setting in bo3macfix.c, see settings_ini.c)
static int read_value(const setting_t *setting) {
    return settings_ini_read(Settings_File, setting->name, setting->fallback, setting->hex ? 16 : 10);
}

bool settings_write(const char *name, const char *value) {
    return settings_ini_write(Settings_File, name, value);
}

// read when the first UI state gets the BO3MacFix table, before the page can change anything
static void remember_startup_values(void) {
    if (startup_known)
        return;
    for (size_t i = 0; i < Setting_Count; i++)
        startup_values[i] = read_value(&settings[i]);
    startup_known = true;
}

static bool restart_pending(void) {
    for (size_t i = 0; i < Setting_Count; i++)
        if (needs_restart[i])
            return true;
    return false;
}

#pragma mark - restarting

static void (*Cbuf_AddText)(int localClientNum, const char *text);

// Starts the game again through Steam once this process has gone, then quits through the game's own quit path (so
// config.ini is saved). Going through Steam keeps its launch options and session; the launcher it runs makes or
// reuses the pre-patched executable, and the game reloads the last mod (AAE) by itself.
static void restart_game(void) {
    // only once: pressing restart again before the game has quit would start it twice
    static bool restarting = false;
    if (restarting)
        return;
    restarting = true;
    char command[256];
    snprintf(command, sizeof(command), "while kill -0 %d 2>/dev/null; do sleep 0.5; done; sleep 1; open steam://rungameid/311210",
             (int)getpid());
    char *argv[] = { "/bin/sh", "-c", command, NULL };
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid;
    int spawned = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    printf("BO3MacFix: restarting the game%s\n", spawned == 0 ? "" : " (couldn't start the relaunch, quitting only)");
    Cbuf_AddText = (void *)(game_base_address + ADDR_Cbuf_AddText);
    Cbuf_AddText(0, "quit\n");
}

#pragma mark - Lua

#define Max_Row_Choices 20

// a choice row's values and labels, and its value now. Game display also lists every connected display by name
// (values 100 and up), so it's asked each time the page opens; one that was chosen but isn't connected stays listed.
static int row_choices(const setting_t *t, int *values, char labels[][64], int *value) {
    int count = 0;
    *value = read_value(t);
    if (strcmp(t->name, "GameDisplay") == 0) {
        count = display_follow_game_display_choices(values, labels, Max_Row_Choices - 1);
        *value = display_follow_game_display_current();
    }
    if (count <= 0) {
        for (count = 0; count < t->count; count++) {
            values[count] = t->values[count];
            strlcpy(labels[count], t->labels[count], sizeof(labels[count]));
        }
    }
    bool listed = false;
    for (int c = 0; c < count; c++)
        listed = listed || values[c] == *value;
    if (!listed && strcmp(t->name, "GameDisplay") == 0) {
        values[count] = *value;
        strlcpy(labels[count++], "Chosen display (not connected)", sizeof(labels[0]));
    }
    // display names go between our separators
    for (int c = 0; c < count; c++)
        for (char *p = labels[c]; *p != '\0'; p++)
            if ((unsigned char)*p < 0x20)
                *p = ' ';
    return count;
}

// one line per setting: name, label, description, category, kind, values, labels, value, default, live, separated
// by \x1f, with values and labels separated by \x1e (the Lua half turns this into tables)
static int lua_list(lua_State *s) {
    size_t size = 16384, used = 0;
    char *text = calloc(1, size);
    for (size_t i = 0; text != NULL && i < Setting_Count; i++) {
        const setting_t *t = &settings[i];
        char values[256] = "", labels[Max_Row_Choices * 65] = "";
        int choice_values[Max_Row_Choices], value;
        char choice_labels[Max_Row_Choices][64];
        int count = row_choices(t, choice_values, choice_labels, &value);
        for (int c = 0; c < count; c++) {
            char v[32];
            snprintf(v, sizeof(v), "%s%d", c ? "\x1e" : "", choice_values[c]);
            strlcat(values, v, sizeof(values));
            if (c)
                strlcat(labels, "\x1e", sizeof(labels));
            strlcat(labels, choice_labels[c], sizeof(labels));
        }
        used += snprintf(text + used, size - used, "%s\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\x1f%s\x1f%d\x1f%d\x1f%d\n", t->name, t->label,
                         t->description, t->category, count ? "choice" : "toggle", values, labels, value, t->fallback,
                         live_now(t) ? 1 : 0);
        if (used >= size)
            break;
    }
    hks_push_string(s, text != NULL ? text : "");
    free(text);
    return 1;
}

// SetSetting(name, value as a string): saves it, returns whether the game needs a restart for it (nil if it couldn't
// be saved)
static int lua_set(lua_State *s) {
    const char *name = hks_check_string(s, 1), *value_text = hks_check_string(s, 2);
    const setting_t *t = name != NULL ? find_setting(name) : NULL;
    char *end = NULL;
    double number = value_text != NULL ? strtod(value_text, &end) : 0;
    // SetSetting(name) (a value of "nil") or one that isn't a number would otherwise save 0
    if (t == NULL || value_text == NULL || end == value_text || !(number >= INT_MIN && number <= INT_MAX)) {
        hks_push_nil(s);
        return 1;
    }
    int value = (int)number;
    size_t index = (size_t)(t - settings);
    remember_startup_values();
    // a display picked by name (Game display 100 and up) is saved as 3, "the chosen display", whose UUID
    // display_follow keeps in displays.ini
    int saved = display_follow_owns(t->name) ? display_follow_persisted_value(t->name, value) : value;
    char text[32];
    if (t->hex && saved >= 0)
        snprintf(text, sizeof(text), "%06X", saved);
    else
        snprintf(text, sizeof(text), "%d", saved);
    if (!settings_write(t->name, text)) {
        printf("BO3MacFix: couldn't save %s to settings.ini\n", t->name);
        hks_push_nil(s);
        return 1;
    }
    bool applied = live_now(t) && apply_live(t, value);
    // compared with the value the game started with, so changing a setting back needs no restart
    bool restart = !applied && saved != startup_values[index];
    needs_restart[index] = restart;
    printf("BO3MacFix: set %s = %s%s\n", t->name, text, applied ? "" : restart ? " (takes effect after a restart)" : "");
    hks_push_boolean(s, restart);
    return 1;
}

static int lua_restart(lua_State *s) {
    restart_game();
    return 0;
}

void settings_restart_game(void) {
    restart_game();
}

static int lua_pending(lua_State *s) {
    hks_push_boolean(s, restart_pending());
    return 1;
}

static const char settings_lua[] =
    "local list, set, restart, pending = ...\n"
    "local function split(text, separator)\n"
    "  local parts, start = {}, 1\n"
    "  while true do\n"
    "    local at = string.find(text, separator, start, true)\n"
    "    if not at then table.insert(parts, string.sub(text, start)) return parts end\n"
    "    table.insert(parts, string.sub(text, start, at - 1))\n"
    "    start = at + 1\n"
    "  end\n"
    "end\n"
    "BO3MacFix = BO3MacFix or {}\n"
    "function BO3MacFix.GetSettings()\n"
    "  local result = {}\n"
    "  for _, line in ipairs(split(list(), \"\\n\")) do\n"
    "    if line ~= \"\" then\n"
    "      local f = split(line, \"\\31\")\n"
    "      local values, labels = {}, {}\n"
    "      if f[6] ~= \"\" then for _, v in ipairs(split(f[6], \"\\30\")) do table.insert(values, tonumber(v)) end end\n"
    "      if f[7] ~= \"\" then labels = split(f[7], \"\\30\") end\n"
    "      table.insert(result, { name = f[1], label = f[2], description = f[3], category = f[4], kind = f[5],\n"
    "        values = values, labels = labels, value = tonumber(f[8]), default = tonumber(f[9]), live = f[10] == \"1\" })\n"
    "    end\n"
    "  end\n"
    "  return result\n"
    "end\n"
    "function BO3MacFix.SetSetting(name, value)\n"
    "  if type(value) == \"boolean\" then value = value and 1 or 0 end\n"
    "  return set(name, tostring(value))\n"
    "end\n"
    "function BO3MacFix.RestartGame() restart() end\n"
    "function BO3MacFix.RestartPending() return pending() end\n";

void settings_lua_install(lua_State *s) {
    remember_startup_values();
    // the stack can be reallocated while our Lua runs, so remember the top as an index
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    if (hks_load_source(s, settings_lua, sizeof(settings_lua) - 1, "=BO3MacFix settings table")) {
        hks_push_cfunction(s, lua_list, "SettingsList");
        hks_push_cfunction(s, lua_set, "SettingsSet");
        hks_push_cfunction(s, lua_restart, "SettingsRestart");
        hks_push_cfunction(s, lua_pending, "SettingsPending");
        hks_pcall(s, 4, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
}
