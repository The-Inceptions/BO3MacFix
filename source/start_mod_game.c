// StartWithMod in the game (start_mod_game.h).
#include "start_mod_game.h"
#include <stdio.h>
#include <string.h>
#include "notices.h"
#include "offsets.h"
#include "start_mod.h"

// from the game's working folder, its MacOS folder in the app (as bo3macfix.c's Macfix_Path and Basegame_Path)
#define Macfix_Folder "../../../BO3MacFix"
#define Game_Folder "../../.."
#define Workshop_Folder "../../../../../workshop"

static const char *loaded_name;          // the game's: the loaded mod's name, char[0x20] ("" without one)
static const volatile int32_t *load_state; // the game's: its mod load, 0 while none is under way (offsets.h)
static start_mod_paths_t paths = { Macfix_Folder, Game_Folder, Workshop_Folder };
static bool debug_on;
static bool look_pending; // a first menu came while the game was loading a mod: look once it's done

void start_mod_game_init(uint64_t game_base_address, bool enabled, const char *folder, const char *launch_note,
                         bool debug) {
    loaded_name = (const char *)(game_base_address + ADDR_Mods_LoadedName);
    load_state = (const volatile int32_t *)(game_base_address + ADDR_Mods_LoadState);
    paths.folder = folder != NULL && folder[0] != '\0' ? folder : Macfix_Folder;
    debug_on = debug;
    const char *state = enabled ? "on" : "off";
    start_mod_plan_t plan;
    char mod[Start_Mod_Name_Size];
    if (launch_note == NULL || launch_note[0] == '\0') {
        printf("BO3MacFix: StartWithMod %s: this start didn't come from a launcher that knows it (an older launcher, "
               "or a start without it)\n", state);
        return;
    }
    if (!start_mod_parse_note(launch_note, &plan, mod)) {
        printf("BO3MacFix: StartWithMod %s: the launcher's note \"%.64s\" means nothing to this library\n", state,
               launch_note);
        return;
    }
    printf("BO3MacFix: StartWithMod %s: %s%s%s\n", state, mod, mod[0] != '\0' ? ": " : "", start_mod_plan_text(plan));
    char title[Start_Mod_Title_Max + 16], text[128];
    start_mod_title(&paths, mod, title, sizeof(title));
    if (start_mod_notice_text(plan, title, text, sizeof(text)))
        notices_post_lasting("%s", text);
}

static const char *loaded_text(start_mod_loaded_t loaded) {
    switch (loaded) {
    case Start_Mod_Loaded_None:
        return "no mod is loaded";
    case Start_Mod_Loaded_Stand_In:
        return "only the stock game's stand-in for custom maps is loaded";
    case Start_Mod_Loaded_Unfit:
        return "the loaded mod's name can't go on a command line";
    default:
        return "a mod is loaded";
    }
}

// the start got as far as the menus with no mod load under way: the marker goes, and lastmod.txt gets the mod loaded
// now. when says what moment it is, for the debug line.
static void look_now(const char *when) {
    char raw[Start_Mod_Name_Size];
    memcpy(raw, loaded_name, sizeof(raw));
    start_mod_look_t look;
    start_mod_menu_ready(&paths, raw, &look);
    if (look.marker_there)
        printf("BO3MacFix: StartWithMod: the start got as far as the menus with no mod load under way%s\n",
               look.marker_removed ? "" : ", but lastmod.pending couldn't be removed");
    const char *before = look.before == Start_Mod_File_Named ? look.before_name
                         : look.before == Start_Mod_File_Damaged ? "a damaged lastmod.txt" : "nothing";
    if (look.changed && look.loaded == Start_Mod_Loaded_Mod)
        printf("BO3MacFix: StartWithMod: remembered %s for the next start (was %s)%s\n", look.loaded_name, before,
               look.written ? "" : ", but lastmod.txt couldn't be written");
    else if (look.changed) {
        char which[Start_Mod_Name_Size + 4] = "";
        if (look.loaded != Start_Mod_Loaded_None)
            snprintf(which, sizeof(which), " (%s)", look.loaded_name);
        printf("BO3MacFix: StartWithMod: forgot %s: %s%s%s\n", before, loaded_text(look.loaded), which,
               look.written ? "" : ", but lastmod.txt couldn't be removed");
    }
    if (debug_on)
        printf("BO3MacFix: StartWithMod: %s: %s (%s), lastmod.txt holds %s%s\n", when, loaded_text(look.loaded),
               look.loaded_name[0] != '\0' ? look.loaded_name : "none", before, look.changed ? ", changed" : "");
}

void start_mod_game_menu_ready(void) {
    if (loaded_name == NULL)
        return;
    // A mod's load builds the menus again and again before it is done (in a start with the mod on the command line the
    // first menu comes about 10 s in and the load is done at about 19 s), and it is the done that counts: until then
    // the look waits for the poll. A start without a mod never loads one, so it looks at its first menu.
    int32_t state = *load_state;
    if (state != 0) {
        if (debug_on && !look_pending)
            printf("BO3MacFix: StartWithMod: first menu while the game loads a mod (state %d): looking once it's done\n",
                   (int)state);
        look_pending = true;
        return;
    }
    look_pending = false;
    look_now("first menu");
}

void start_mod_game_poll(void) {
    // a look is only left for later once this is set up (start_mod_game_menu_ready)
    if (!look_pending || *load_state != 0)
        return;
    look_pending = false;
    look_now("the mod's load is done");
}
