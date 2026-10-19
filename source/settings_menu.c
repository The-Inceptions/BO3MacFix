// The in-game settings page (settings_menu.lua), set up in each UI Lua state the game creates.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "checkpoint.h"
#include "havok.h"
#include "notices.h"
#include "settings.h"
#include "settings_menu.h"
#include "settings_menu_lua.h"

// BO3MacFix.Log(text): the page's messages go in BO3MacFix's log
static int lua_log(lua_State *s) {
    const char *text = hks_check_string(s, 1);
    printf("BO3MacFix: %s\n", text != NULL ? text : "");
    return 0;
}

// the zombies checkpoint (checkpoint.c), BO3MacFix.SaveCheckpoint and the rest below
static const checkpoint_paths_t checkpoint_paths = { Checkpoint_File, Checkpoint_Players_Folder };

// SaveCheckpoint("Key=value" lines): whether it was saved
static int lua_checkpoint_save(lua_State *s) {
    bool saved = checkpoint_save(&checkpoint_paths, hks_check_string(s, 1), (long)time(NULL));
    printf("BO3MacFix: checkpoint: %s\n", saved ? "saved" : "couldn't save it");
    hks_push_boolean(s, saved);
    return 1;
}

// ReadCheckpoint(): the checkpoint as "Key=value" lines, empty if there's none
static int lua_checkpoint_read(lua_State *s) {
    static char text[8192];
    checkpoint_read(&checkpoint_paths, text, sizeof(text));
    hks_push_string(s, text);
    return 1;
}

// ApplyCheckpoint(preset, previous round, previous points): the console commands to run, nil if it couldn't
static int lua_checkpoint_apply(lua_State *s) {
    const char *preset = hks_check_string(s, 1), *round = hks_check_string(s, 2), *points = hks_check_string(s, 3);
    char commands[512];
    bool applied = checkpoint_apply(&checkpoint_paths, preset, round, points, commands, sizeof(commands));
    printf("BO3MacFix: checkpoint: %s\n", applied ? "the next game resumes it" : "couldn't set up the resume");
    if (applied)
        hks_push_string(s, commands);
    else
        hks_push_nil(s);
    return 1;
}

// ClearCheckpoint(current round, current points): the console commands to run, nil if no resume was set up
static int lua_checkpoint_clear(lua_State *s) {
    const char *round = hks_check_string(s, 1), *points = hks_check_string(s, 2);
    char commands[512];
    bool cleared = checkpoint_clear(&checkpoint_paths, round, points, commands, sizeof(commands));
    if (cleared) {
        printf("BO3MacFix: checkpoint: resume undone, the starting round and points are the player's own again\n");
        hks_push_string(s, commands);
    } else {
        hks_push_nil(s);
    }
    return 1;
}

// CheckpointResume(mark): 0 no resume set up, 1 set up, 2 a game has started with it; with mark, notes that one has
static int lua_checkpoint_resume(lua_State *s) {
    float mark = 0;
    if (hks_number_arg(s, 1, &mark) && mark != 0 && checkpoint_mark_in_use(&checkpoint_paths))
        printf("BO3MacFix: checkpoint: a game started with the resume\n");
    hks_push_number(s, (float)checkpoint_resume_state(&checkpoint_paths));
    return 1;
}

static const char checkpoint_lua[] =
    "local save, read, apply, clear, resume = ...\n"
    "BO3MacFix.SaveCheckpoint, BO3MacFix.ReadCheckpoint, BO3MacFix.ApplyCheckpoint = save, read, apply\n"
    "BO3MacFix.ClearCheckpoint, BO3MacFix.CheckpointResume = clear, resume\n";

static void checkpoint_lua_install(lua_State *s) {
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    if (hks_load_source(s, checkpoint_lua, sizeof(checkpoint_lua) - 1, "=BO3MacFix checkpoint")) {
        hks_push_cfunction(s, lua_checkpoint_save, "CheckpointSave");
        hks_push_cfunction(s, lua_checkpoint_read, "CheckpointRead");
        hks_push_cfunction(s, lua_checkpoint_apply, "CheckpointApply");
        hks_push_cfunction(s, lua_checkpoint_clear, "CheckpointClear");
        hks_push_cfunction(s, lua_checkpoint_resume, "CheckpointResume");
        hks_pcall(s, 5, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
}

static int lua_install(lua_State *s) {
    settings_lua_install(s);
    notices_lua_install(s);
    checkpoint_lua_install(s);
    return 0;
}

// runs settings_menu.lua; the argument says whether to open the page by itself (BO3MACFIX_OPENSETTINGS=1, for testing)
static int lua_load_menu(lua_State *s) {
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    if (hks_load_source(s, (const char *)settings_menu_lua, settings_menu_lua_len, "=BO3MacFix settings menu")) {
        const char *open = getenv("BO3MACFIX_OPENSETTINGS");
        hks_push_boolean(s, open != NULL && atoi(open) != 0);
        hks_pcall(s, 1, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
    return 0;
}

// Called from Engine.GetBuildStringField and Engine.Exec, which run in several Lua states: the UI's (All-around
// Enhancement's version check while its scripts load; the stock main menu's "checkforinvites") and the lobby's (no
// menus there). The game also makes a new UI state more than once on the way to the main menu, sometimes at the same
// address. So every call runs this small chunk, which does nothing in a state without menus or one that already has
// the page. Otherwise it adds the BO3MacFix table and has CoD.Menu.NewForUIEditor, which every menu is made with, load
// the page when the next menu is made and pass it each menu. Doing more in here, like compiling the page, broke All-
// around Enhancement's start-up; and the page loads in a coroutine, so compiling and running it grow the coroutine's
// Lua stack, not the stack of the code making the menu.
static const char start_lua[] =
    "local log, install, load_menu = ...\n"
    "local table = rawget(_G, 'BO3MacFix')\n"
    "if table ~= nil and table.Log ~= nil then return end\n"
    "if CoD == nil or CoD.Menu == nil or CoD.Menu.NewForUIEditor == nil then return end\n"
    // by the time the stock main menu is made, the UI's scripts have turned off making new globals
    "if table == nil then rawset(_G, 'BO3MacFix', {}) end\n"
    "install()\n"
    "if BO3MacFix == nil then return end\n"
    "BO3MacFix.Log = log\n"
    "local new_menu, loaded = CoD.Menu.NewForUIEditor, false\n"
    "CoD.Menu.NewForUIEditor = function(name, ...)\n"
    "  if not loaded then\n"
    "    loaded = true\n"
    "    local ok, err\n"
    "    if coroutine ~= nil then\n"
    "      ok, err = coroutine.resume(coroutine.create(function() load_menu() end))\n"
    "    else\n"
    "      ok, err = pcall(load_menu)\n"
    "    end\n"
    "    if not ok then log('settings menu error: ' .. tostring(err)) end\n"
    "  end\n"
    "  local menu = new_menu(name, ...)\n"
    "  if BO3MacFix.MenuCreated ~= nil then\n"
    "    local ok, err = pcall(BO3MacFix.MenuCreated, name, menu)\n"
    "    if not ok then log('settings menu error: ' .. tostring(err)) end\n"
    "  end\n"
    "  return menu\n"
    "end\n";

void settings_menu_run(lua_State *s) {
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    if (hks_load_source(s, start_lua, sizeof(start_lua) - 1, "=BO3MacFix settings start")) {
        hks_push_cfunction(s, lua_log, "SettingsLog");
        hks_push_cfunction(s, lua_install, "SettingsInstall");
        hks_push_cfunction(s, lua_load_menu, "SettingsLoadMenu");
        hks_pcall(s, 3, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
}
