// T7Overcharged's smaller pieces that All-around Enhancement uses, done the way T7Overcharged does them (see
// t7o_small.c): its customkeymap command, keys that call a function of the mod's Lua, the debounce of its
// SendMenuResponse command, the dvars it changes when it starts, and its other commands the mod runs. The rules are
// plain functions here, so they can be tested without the game; the game's side is at the end.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- numbers as T7Overcharged reads them, with std::stoi: strtol in base 10 (blanks first, a sign, and anything
// after the digits are allowed), on Windows, where a long is an int

typedef enum {
    T7o_Number_Ok,
    T7o_Number_Invalid, // no digits: std::stoi throws std::invalid_argument
    T7o_Number_Range,   // beyond an int: std::stoi throws std::out_of_range
} t7o_number;
t7o_number t7o_stoi(const char *text, int *value);

// ---- customkeymap <name> <keys> [debounce ms]

#define T7o_Keys 256          // the game's key numbers: 27 is ESCAPE, 200 MOUSE1 (its keynames table, in both games)
#define T7o_Keymap_Max 32     // mappings kept; T7Overcharged's map has no limit, All-around Enhancement makes two
#define T7o_Keymap_Name_Max 64
#define T7o_Keymap_Debounce_Ms 1000 // without a third number

// a set of the game's key numbers, as T7Overcharged's std::bitset<256>
typedef struct {
    uint64_t bits[T7o_Keys / 64];
} t7o_keyset;
void t7o_keyset_add(t7o_keyset *set, int key); // a key from 0 to 255; others are left out
bool t7o_keyset_has(const t7o_keyset *set, int key);
bool t7o_keyset_equal(const t7o_keyset *a, const t7o_keyset *b);

typedef struct {
    t7o_keyset keys;
    char name[T7o_Keymap_Name_Max];
    int debounce_ms;
} t7o_keymapping;

// T7Overcharged's debounce: a function-local static, set up at the first call, so one for all mappings
typedef struct {
    bool started, first;
    int debounce_ms;
    int64_t last_ns;
} t7o_debounce;

typedef struct {
    t7o_keymapping mappings[T7o_Keymap_Max];
    int count;
    t7o_debounce debounce;
} t7o_keymap;

typedef enum {
    T7o_Keymap_Added,
    T7o_Keymap_Replaced,   // the same keys again: the new name and debounce take the old one's place
    T7o_Keymap_Usage,      // fewer than three words: T7Overcharged prints its usage
    T7o_Keymap_Bad_Number, // a key or the debounce that isn't a number std::stoi reads: T7Overcharged throws
    T7o_Keymap_Bad_Name,   // a name that would end the quoted string of the Lua it goes into
    T7o_Keymap_Full,
} t7o_keymap_result;

// argv[0] is the command's name, argv[1] the name, argv[2] the keys: numbers joined by '+' (an empty part is not a
// number; a trailing '+' ends the list, as std::getline does), argv[3] the debounce. Keys above 255, and negative
// ones, are left out; a mapping without keys is kept and never fires, as in T7Overcharged.
t7o_keymap_result t7o_keymap_command(t7o_keymap *map, int argc, const char *const *argv);

// whether a queued input event can fire a mapping: a key event (type 1, SE_KEY), the key's release (value2 is 1 when
// it goes down, 0 when it comes up), the game not quitting, local client 0's console closed (key catcher 1), a key
// below 256
bool t7o_keymap_event_counts(int type, int value2, int key, int quit_in_progress, int key_catchers);
// the mapping whose keys are exactly the held ones plus the key, NULL for none
const t7o_keymapping *t7o_keymap_find(const t7o_keymap *map, const t7o_keyset *held, int key);
// whether a mapping that was found fires now: the debounce is the one of the first mapping that ever fired, and
// counts from the last time any mapping fired
bool t7o_keymap_debounce_passes(t7o_debounce *debounce, int mapping_debounce_ms, int64_t now_ns);
// the Lua T7Overcharged runs for a mapping (its text exactly, with the name); false when it doesn't fit
bool t7o_keymap_lua(char *buffer, size_t size, const char *name);

// ---- SendMenuResponse <menu> <response> [debounce ms]

#define T7o_MenuResponse_Debounce_Ms 1000 // without a third number
// the debounce of a call: argv[3] read with std::stoi, or 1000 without one; not T7o_Number_Ok where T7Overcharged
// throws (nothing is sent then and nothing changes)
t7o_number t7o_menuresponse_debounce(int argc, const char *const *argv, int *debounce_ms);
// whether a call with at least a menu and a response goes on: T7Overcharged keeps one time for all SendMenuResponse
// calls, set by every call it lets through (whether or not that one then sends anything), and each call's own
// debounce decides
bool t7o_menuresponse_debounce_passes(t7o_debounce *debounce, int call_debounce_ms, int64_t now_ns);

// ---- dvars T7Overcharged changes by itself when it starts

// one change: the dvar's flags set to flags (when set_flags; for a dvar that has one per session mode, the current
// mode's), then its value set as with Dvar_SetFromStringByName(name, value, true) (when value isn't NULL)
typedef struct {
    const char *name;
    bool set_flags;
    uint32_t flags;
    const char *value;
} t7o_dvar_change;
// its patches component, at once: what a host sends (sv_maxRate, sv_network_fps) and votes, map switching and voice
extern const t7o_dvar_change t7o_network_defaults[];
extern const size_t t7o_network_default_count;

// its dvar_patches component, once, T7o_Dvar_Patch_Delay_S seconds after it starts: 35 dvars get fixed flags (so
// cg_fovscale, r_fog and the like can be set from the console without cheats, and some are saved in the config)
#define T7o_Dvar_Patch_Delay_S 10
extern const t7o_dvar_change t7o_dvar_patches[];
extern const size_t t7o_dvar_patch_count;

// the session mode as the game's Com_SessionMode_GetMode gives it: the state's low 4 bits, with their sign
int t7o_session_mode(uint32_t session_mode_state);
// the dvar whose flags a change sets: for a dvar of the per-session-mode type (15), the one of the mode, or the dvar
// itself when the mode has none (Dvar_GetSessionModeSpecificDvar); NULL for a mode other than 0 to 2, for which
// the game's function may stop the game with an error
uint8_t *t7o_mode_dvar(uint8_t *dvar, int mode);

// ---- DB_FreeUnusedResources [first second]

// the game's DB_FreeUnusedResources(first, second) as T7Overcharged's command calls it: the two numbers read with
// std::stoi, each true when it isn't 0; with fewer than three words (false, true). Not T7o_Number_Ok where
// T7Overcharged throws (nothing is called then)
t7o_number t7o_free_unused_args(int argc, const char *const *argv, bool *first, bool *second);

// ---- the game's side

// at start-up: the console commands (customkeymap, cursoroverride, updateVidConfig, DB_FreeUnusedResources) and the
// hook on the game's input events
void t7o_small_install(uint64_t base_address);
// SendMenuResponse (t7overcharged.c): whether this call goes on, by T7Overcharged's debounce
bool t7o_small_menuresponse_passes(int argc, const char *const *argv);
// the mod has loaded T7Overcharged (the stand-in's init, t7overcharged.c): the first time, what T7Overcharged changes
// in the game by itself when it starts
void t7o_small_mod_loaded(void);
