// StartWithMod: the game starts in the mod it was last quit in (start_mod.c).
//
// The player's start today: the stock game's menus come up, the player loads the mod from MODS, and the game unloads
// its menus, loads the mod and builds the menus twice more behind a spinner. Started with "+set fs_game <mod>" on its
// command line (as the test runner has always started it) the game loads the mod before its first menu and comes up
// in the mod's own start screen, about 15 seconds sooner.
//
// So the game, with BO3MacFix in it, notes which mod is loaded in BO3MacFix/lastmod.txt each time the first menu of
// its UI is up (start_mod_game.c), and the launcher (launcher/launcher.m, the program Steam starts) puts
// "+set fs_game <that mod>" on the game's command line at the next start. Before such a start the launcher leaves a
// marker that the game removes at its first menu: a start with the mod that never got that far (the mod stops the
// game at its start, its files are damaged) makes the next start the stock game, with a notice, and forgets the mod.
//
// Plain C and POSIX files only: the library and the launcher build this same file, and tests/test_start_mod.c tests
// it in folders of its own.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// The game keeps fs_game and the loaded mod's name in 0x20 bytes, so a name has at most 31 characters
#define Start_Mod_Name_Size 32
// the files, in BO3MacFix's folder: the mod loaded at the last quit, and the marker of a start given it
#define Start_Mod_Memory_File "lastmod.txt"
#define Start_Mod_Marker_File "lastmod.pending"
// the stand-in the stock game loads for a custom map when no mod is loaded (its Lua's LuaEnums.DEFAULT_MOD_NAME)
#define Start_Mod_Stand_In "usermaps"
// Steam's id of the game: its workshop items are in steamapps/workshop/content/311210/<item>
#define Start_Mod_App_Id "311210"
// the most of a mod's title a notice shows (a notice holds 127 characters)
#define Start_Mod_Title_Max 32
// what the launcher tells the game about its start, in the game's environment: "<what> <mod>" (start_mod_note)
#define Start_Mod_Launch_Env "BO3MACFIX_STARTWITHMOD_LAUNCH"

// where the files are
typedef struct {
    const char *folder;   // lastmod.txt and lastmod.pending: BO3MacFix's folder, or BO3MACFIX_STARTWITHMOD_DIR
    const char *game;     // the game's folder: a mod of its own is in mods/<name>/zone/
    const char *workshop; // Steam's steamapps/workshop: a workshop item is in content/311210/<id>/
} start_mod_paths_t;

// whether name may be remembered and put on the game's command line: 1 to 31 letters, digits, '_', '-' and '.', not
// beginning with '.' or '-', without "..", and not the stand-in. The game's fs_game allows more (a space, ';'), but
// on a command line a space starts another word and ';' another command.
bool start_mod_name_ok(const char *name);
// whether name is the stock game's stand-in for custom maps ("usermaps", in any case)
bool start_mod_is_stand_in(const char *name);

// what the game shows as loaded, from its 0x20 bytes at ADDR_Mods_LoadedName
typedef enum {
    Start_Mod_Loaded_None,     // no mod
    Start_Mod_Loaded_Stand_In, // the stand-in for custom maps: the stock game, never remembered
    Start_Mod_Loaded_Unfit,    // a mod whose name can't go on a command line (or no name at all in the 0x20 bytes)
    Start_Mod_Loaded_Mod,      // a mod to remember
} start_mod_loaded_t;
// name gets the name, with anything but printable ASCII as '?' for an unfit one (for the log)
start_mod_loaded_t start_mod_loaded(const char raw[Start_Mod_Name_Size], char name[Start_Mod_Name_Size]);

// what lastmod.txt (or the marker) holds
typedef enum {
    Start_Mod_File_Absent,  // no file, or an empty one: no mod
    Start_Mod_File_Named,   // a name and a line end
    Start_Mod_File_Damaged, // anything else (not ours, cut short, too long, not readable): no mod either
} start_mod_file_t;
// the file's text, length bytes that need not end in a 0
start_mod_file_t start_mod_parse(const char *text, size_t length, char name[Start_Mod_Name_Size]);
// the file at path
start_mod_file_t start_mod_read(const char *path, char name[Start_Mod_Name_Size]);
// makes the file at path hold name and a line end, written to a temporary file that then replaces it, so it is never
// half written; "" removes it. False if that failed (or name isn't one that may be remembered).
bool start_mod_write(const char *path, const char *name);

// whether a mod's files are there for the game to load
typedef enum {
    Start_Mod_Files_There,    // its core_mod.ff, in the workshop item's folder or in mods/<name>/zone/
    Start_Mod_Files_Missing,  // neither
    Start_Mod_Files_Updating, // a workshop item that Steam is downloading right now (workshop/downloads/311210/<id>)
} start_mod_files_t;
start_mod_files_t start_mod_files(const start_mod_paths_t *paths, const char *name);

// what the launcher does about the remembered mod
typedef enum {
    Start_Mod_Off,      // StartWithMod is off (or NoCodePatches is on)
    Start_Mod_Nothing,  // no mod was loaded at the last quit
    Start_Mod_Damaged,  // lastmod.txt holds no name of ours: ignored
    Start_Mod_Failed,   // the last start with the mod didn't finish loading it: the stock game, the mod forgotten
    Start_Mod_Busy,     // the command line has an fs_game of its own or starts a map
    Start_Mod_Missing,  // the mod's files aren't there
    Start_Mod_Updating, // Steam is downloading the mod
    Start_Mod_Unsafe,   // the marker couldn't be written, and without it a start that fails could come back each time
    Start_Mod_Given,    // start with it: "+set fs_game <mod>"
} start_mod_plan_t;

// The launcher's part, before it starts the game: what to do, with mod the mod it is about (the marker's for
// Failed, "" when a damaged marker named none). Given writes the marker; Failed removes lastmod.txt and the marker.
// With write false nothing is written (a dry run with no folder of its own): Given then leaves no marker.
start_mod_plan_t start_mod_launch(const start_mod_paths_t *paths, bool enabled, int argc, char *const argv[],
                                  bool write, char mod[Start_Mod_Name_Size]);
// the launcher couldn't start the game it was to start with the mod: its marker goes again
bool start_mod_launch_abandoned(const start_mod_paths_t *paths);

// what the launcher tells the game: "<what> <mod>" (what: off, nothing, damaged, failed, busy, missing, updating,
// unsafe, given), into out
void start_mod_note(start_mod_plan_t plan, const char *mod, char *out, size_t size);
// and the game reads it back; false for anything else, which the game then ignores
bool start_mod_parse_note(const char *note, start_mod_plan_t *plan, char mod[Start_Mod_Name_Size]);
// a few words on a plan, for the logs ("started with it", "Steam is downloading it" ...)
const char *start_mod_plan_text(start_mod_plan_t plan);

// The game's part at the first menu of each UI state: the start got as far as the menus, so the marker goes, and
// lastmod.txt is made to hold the mod the game shows as loaded (or removed). Touches the files only when that changes
// something.
typedef struct {
    bool marker_there, marker_removed;   // the marker was there, and went
    start_mod_loaded_t loaded;           // what the game shows as loaded
    char loaded_name[Start_Mod_Name_Size];
    start_mod_file_t before;             // what lastmod.txt held
    char before_name[Start_Mod_Name_Size];
    bool changed, written;               // lastmod.txt had to change, and it did
} start_mod_look_t;
void start_mod_menu_ready(const start_mod_paths_t *paths, const char raw_loaded[Start_Mod_Name_Size],
                          start_mod_look_t *look);

// a workshop item's title from its workshop.json ("Title"), without the game's colour codes (^1), cut to
// Start_Mod_Title_Max bytes at a word; false if it has none
bool start_mod_title_from_json(const char *json, size_t length, char *out, size_t size);
// the mod as the player knows it, for a notice: a workshop item's title, else "the mod <name>"
void start_mod_title(const start_mod_paths_t *paths, const char *name, char *out, size_t size);
// the notice for a start the launcher didn't give the mod (Failed, Missing, Updating), at most 127 characters with
// any title of start_mod_title; false for the others, which get none
bool start_mod_notice_text(start_mod_plan_t plan, const char *title, char *out, size_t size);
