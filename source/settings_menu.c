// The in-game settings page (settings_menu.lua), set up in each UI Lua state the game creates.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "checkpoint.h"
#include "checkpoint_autosave.h"
#include "checkpoint_lua.h"
#include "havok.h"
#include "notices.h"
#include "settings.h"
#include "settings_menu.h"
#include "settings_menu_lua.h"
#include "zm_inventory.h"

// BO3MacFix.Log(text): the page's messages go in BO3MacFix's log
static int lua_log(lua_State *s) {
    const char *text = hks_check_string(s, 1);
    printf("BO3MacFix: %s\n", text != NULL ? text : "");
    return 0;
}

// saving and resuming a zombies game (checkpoint.c, zm_inventory.c): the C functions checkpoint.lua gets when it loads,
// as BO3MacFix.SaveCheckpoint and the rest
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

// ApplyCheckpoint(preset): the console commands to run, nil if it couldn't
static int lua_checkpoint_apply(lua_State *s) {
    char commands[512];
    bool applied = checkpoint_apply(&checkpoint_paths, hks_check_string(s, 1), commands, sizeof(commands));
    printf("BO3MacFix: checkpoint: %s\n", applied ? "the next game resumes it" : "couldn't set up the resume");
    if (applied)
        hks_push_string(s, commands);
    else
        hks_push_nil(s);
    return 1;
}

// ClearCheckpoint("dvar=value" lines with the options' current values): the console commands to run, nil if no resume
// was set up
static int lua_checkpoint_clear(lua_State *s) {
    char commands[512];
    if (checkpoint_clear(&checkpoint_paths, hks_check_string(s, 1), commands, sizeof(commands))) {
        printf("BO3MacFix: checkpoint: resume undone, the starting options are the player's own again\n");
        hks_push_string(s, commands);
    } else {
        hks_push_nil(s);
    }
    return 1;
}

// CheckpointResume(mark): 0 no resume set up, 1 set up, 2 a game has started with it, 3 that game got its weapons
// back; mark 1 notes that a game has started with it, mark 2 that it got its weapons
static int lua_checkpoint_resume(lua_State *s) {
    float mark = 0;
    if (hks_number_arg(s, 1, &mark) && mark == 1 && checkpoint_mark_in_use(&checkpoint_paths))
        printf("BO3MacFix: checkpoint: a game started with the resume\n");
    if (mark == 2 && checkpoint_mark_weapons_given(&checkpoint_paths))
        printf("BO3MacFix: checkpoint: this game has had its weapons back\n");
    hks_push_number(s, (float)checkpoint_resume_state(&checkpoint_paths));
    return 1;
}

// CheckpointWeapons(): the local player's weapons as "name:type" separated by commas, nil without a game hosted here
static int lua_checkpoint_weapons(lua_State *s) {
    char weapons[1024];
    if (zm_inventory_read(weapons, sizeof(weapons)))
        hks_push_string(s, weapons);
    else
        hks_push_nil(s);
    return 1;
}

// CheckpointWeaponCommands(saved, held): the take and give commands that turn held into saved, nil if malformed
static int lua_checkpoint_weapon_commands(lua_State *s) {
    char commands[1024];
    if (checkpoint_weapon_commands(hks_check_string(s, 1), hks_check_string(s, 2), commands, sizeof(commands)) >= 0)
        hks_push_string(s, commands);
    else
        hks_push_nil(s);
    return 1;
}

// CheckpointAllowCheats(count): lets the next count give and take commands past the game's cheat check
static int lua_checkpoint_allow_cheats(lua_State *s) {
    float count = 0;
    if (hks_number_arg(s, 1, &count) && count > 0 && count <= 32) {
        zm_inventory_allow_cheats((int)count);
        printf("BO3MacFix: checkpoint: letting %d give and take commands through\n", (int)count);
    }
    return 0;
}

// CheckpointLoadout(): the local player's full loadout as "Guns=...\nGunInHand=...\nPerkList=..." lines (zm_loadout.h),
// then PapLevels, Items and GunAmmo lines when there are any,
// nil without a game hosted here
static int lua_checkpoint_loadout(lua_State *s) {
    static loadout_t loadout;
    static char guns[1024], perks[1024], pap[1024], items[1024], ammo[1024], lines[5504];
    char note[256];
    if (!zm_inventory_read_loadout(&loadout, note, sizeof(note))) {
        hks_push_nil(s);
        return 1;
    }
    if (note[0] != '\0')
        printf("BO3MacFix: checkpoint: loadout: %s\n", note);
    if (!loadout_format_guns(&loadout, guns, sizeof(guns)) || !loadout_format_perks(&loadout, perks, sizeof(perks)) ||
        !loadout_format_pap_levels(&loadout, pap, sizeof(pap)) || !loadout_format_items(&loadout, items, sizeof(items)) ||
        !loadout_format_gun_ammo(&loadout, ammo, sizeof(ammo))) {
        printf("BO3MacFix: checkpoint: the loadout doesn't fit in checkpoint.ini\n");
        hks_push_nil(s);
        return 1;
    }
    snprintf(lines, sizeof(lines), "Guns=%s\nGunInHand=%s\nPerkList=%s%s%s%s%s%s%s", guns,
             loadout.held >= 0 ? loadout.guns[loadout.held].name : "", perks, pap[0] != '\0' ? "\nPapLevels=" : "", pap,
             items[0] != '\0' ? "\nItems=" : "", items, ammo[0] != '\0' ? "\nGunAmmo=" : "", ammo);
    hks_push_string(s, lines);
    return 1;
}

// CheckpointRestoreLoadout(): sets the checkpoint's full loadout up for zm_inventory.c to give back, and returns the
// give command that does it; nil if the checkpoint has none
static int lua_checkpoint_restore_loadout(lua_State *s) {
    static loadout_t loadout;
    if (!checkpoint_loadout(&checkpoint_paths, &loadout)) {
        hks_push_nil(s);
        return 1;
    }
    hks_push_string(s, zm_inventory_set_restore(&loadout));
    return 1;
}

// CheckpointRestoreResult(): once the loadout's give command has run, what it did (loadout_result_text); nil before
static int lua_checkpoint_restore_result(lua_State *s) {
    char text[512];
    if (zm_inventory_restore_result(text, sizeof(text)))
        hks_push_string(s, text);
    else
        hks_push_nil(s);
    return 1;
}

// CheckpointTestLoadout(guns, perks, pap): for the test switches, a Guns and a PerkList line set up the same way, the
// first gun in hand, or given the Pack-a-Punch machine's way if pap is true; the give command, nil if malformed
static int lua_checkpoint_test_loadout(lua_State *s) {
    static loadout_t loadout;
    HksObject *pap = HKS_BASE(s) + 2;
    bool pap_test = pap < HKS_TOP(s) && (pap->t & 0xF) == HKS_TBOOLEAN && pap->v.boolean;
    if (!loadout_parse(hks_check_string(s, 1), NULL, hks_check_string(s, 2), &loadout)) {
        hks_push_nil(s);
        return 1;
    }
    loadout.held = loadout.gun_count > 0 ? 0 : -1;
    hks_push_string(s, pap_test ? zm_inventory_set_pap_test(&loadout) : zm_inventory_set_restore(&loadout));
    return 1;
}

// CheckpointNotice(text): shows text in the game, like BO3MacFix's other notices
static int lua_checkpoint_notice(lua_State *s) {
    const char *text = hks_check_string(s, 1);
    if (text != NULL)
        notices_post("%s", text);
    return 0;
}

// ---- AutoSave (checkpoint_autosave.c): its background writer saves to checkpoint.ini too, so the bindings above that
// read or write it are pushed as the locked_ ones below, which hold checkpoint_lock(). Their string arguments are read
// before it's taken: a Lua error for a wrong one would leave the lock held.

#define Autosave_Interval_Ms 15000 // the least time between two automatic saves

static int locked_checkpoint_save(lua_State *s) {
    hks_check_string(s, 1);
    checkpoint_lock();
    // this save is newer than an automatic one the writer hasn't written yet
    checkpoint_autosave_cancel();
    int results = lua_checkpoint_save(s);
    checkpoint_unlock();
    return results;
}

static int locked_checkpoint_read(lua_State *s) {
    checkpoint_lock();
    int results = lua_checkpoint_read(s);
    checkpoint_unlock();
    return results;
}

static int locked_checkpoint_apply(lua_State *s) {
    hks_check_string(s, 1);
    checkpoint_lock();
    int results = lua_checkpoint_apply(s);
    checkpoint_unlock();
    return results;
}

static int locked_checkpoint_clear(lua_State *s) {
    hks_check_string(s, 1);
    checkpoint_lock();
    int results = lua_checkpoint_clear(s);
    checkpoint_unlock();
    return results;
}

// the timer asks CheckpointResume(0) twice a second, so that never waits for the writer: while the writer has the lock
// the answer is the last one, which the writer doesn't change
static atomic_int last_resume_state;
static int locked_checkpoint_resume(lua_State *s) {
    float mark = 0;
    if (hks_number_arg(s, 1, &mark) && mark != 0) {
        checkpoint_lock();
    } else if (!checkpoint_trylock()) {
        hks_push_number(s, (float)atomic_load(&last_resume_state));
        return 1;
    }
    int results = lua_checkpoint_resume(s);
    HksObject *answer = HKS_TOP(s) - 1;
    if (results == 1 && (answer->t & 0xF) == HKS_TNUMBER)
        atomic_store(&last_resume_state, (int)answer->v.number);
    checkpoint_unlock();
    return results;
}

// reads the checkpoint's loadout from checkpoint.ini
static int locked_checkpoint_restore_loadout(lua_State *s) {
    checkpoint_lock();
    int results = lua_checkpoint_restore_loadout(s);
    checkpoint_unlock();
    return results;
}

// the argument at index if it's a string, else NULL (without the Lua error hks_check_string raises)
static const char *string_arg(lua_State *s, int index) {
    HksObject *arg = HKS_BASE(s) + (index - 1);
    return arg < HKS_TOP(s) && (arg->t & 0xF) == HKS_TSTRING ? hks_check_string(s, index) : NULL;
}

// CheckpointAutoSave(lines, note): gives this game ("Key=value" lines, as SaveCheckpoint takes) to the background
// writer, with a note for the log, true if it took it; with no arguments, whether AutoSave is on
static int lua_checkpoint_autosave(lua_State *s) {
    const char *lines = string_arg(s, 1), *note = string_arg(s, 2);
    hks_push_boolean(s, lines != NULL ? checkpoint_autosave_post(lines, note) : checkpoint_autosave_enabled());
    return 1;
}

// CheckpointDelete(): whether the saved game is gone (not while a resume is set up)
static int lua_checkpoint_delete(lua_State *s) {
    bool deleted = checkpoint_delete(&checkpoint_paths);
    printf("BO3MacFix: checkpoint: %s\n", deleted ? "deleted the saved game" : "couldn't delete the saved game");
    hks_push_boolean(s, deleted);
    return 1;
}

// what the writer did with an automatic save, on its queue
static void autosave_report(autosave_outcome_t outcome, long round, long kept, const char *note) {
    switch (outcome) {
    case Autosave_Saved:
        printf("BO3MacFix: checkpoint: AutoSave saved round %ld: %s\n", round, note);
        notices_post("Saved round %ld", round);
        break;
    case Autosave_Kept_Deeper:
        printf("BO3MacFix: checkpoint: AutoSave kept the game saved at round %ld, which this one didn't resume (round "
               "%ld)\n", kept, round);
        break;
    case Autosave_Resuming:
        printf("BO3MacFix: checkpoint: AutoSave waits for the resumed game's weapons (round %ld)\n", round);
        break;
    case Autosave_Failed:
        printf("BO3MacFix: checkpoint: AutoSave couldn't save round %ld\n", round);
        break;
    }
}

// pushes the environment variable's value, or nil
static void push_env(lua_State *s, const char *name) {
    const char *value = getenv(name);
    if (value != NULL && value[0] != '\0')
        hks_push_string(s, value);
    else
        hks_push_nil(s);
}

static int lua_install(lua_State *s) {
    settings_lua_install(s);
    notices_lua_install(s);
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
    // then checkpoint.lua, given its C functions here rather than when the state is set up: compiling and running any
    // more Lua in lua_install, while All-around Enhancement's scripts load, makes AAE quit the game at a map's load.
    // Then the test switches: save so many seconds into a match, give these weapons first, log what the match has
    // so many seconds in. Then AutoSave's writer and deleting the save, and its test switch: how many seconds of a
    // match only set the round it starts at. Then the full loadout's functions, and its test switch: give these perks
    // first.
    checkpoint_autosave_init(&checkpoint_paths, Autosave_Interval_Ms, autosave_report);
    if (hks_load_source(s, (const char *)checkpoint_lua, checkpoint_lua_len, "=BO3MacFix checkpoint")) {
        hks_push_cfunction(s, locked_checkpoint_save, "CheckpointSave");
        hks_push_cfunction(s, locked_checkpoint_read, "CheckpointRead");
        hks_push_cfunction(s, locked_checkpoint_apply, "CheckpointApply");
        hks_push_cfunction(s, locked_checkpoint_clear, "CheckpointClear");
        hks_push_cfunction(s, locked_checkpoint_resume, "CheckpointResume");
        hks_push_cfunction(s, lua_checkpoint_weapons, "CheckpointWeapons");
        hks_push_cfunction(s, lua_checkpoint_weapon_commands, "CheckpointWeaponCommands");
        hks_push_cfunction(s, lua_checkpoint_allow_cheats, "CheckpointAllowCheats");
        hks_push_cfunction(s, lua_checkpoint_notice, "CheckpointNotice");
        push_env(s, "BO3MACFIX_CHECKPOINT_SAVE");
        push_env(s, "BO3MACFIX_CHECKPOINT_GIVE");
        push_env(s, "BO3MACFIX_CHECKPOINT_REPORT");
        hks_push_cfunction(s, lua_checkpoint_autosave, "CheckpointAutoSave");
        hks_push_cfunction(s, lua_checkpoint_delete, "CheckpointDelete");
        push_env(s, "BO3MACFIX_AUTOSAVE_START");
        hks_push_cfunction(s, lua_checkpoint_loadout, "CheckpointLoadout");
        hks_push_cfunction(s, locked_checkpoint_restore_loadout, "CheckpointRestoreLoadout");
        hks_push_cfunction(s, lua_checkpoint_restore_result, "CheckpointRestoreResult");
        hks_push_cfunction(s, lua_checkpoint_test_loadout, "CheckpointTestLoadout");
        push_env(s, "BO3MACFIX_CHECKPOINT_PERKS");
        hks_pcall(s, 20, 0);
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
