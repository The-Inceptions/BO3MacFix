// ClientFieldGuard: a script that asks for a client field nobody registered gets a script error instead of crashing
// the game.
//
// Client fields are named values the server's scripts (GSC) send to the clients: per entity, per player, for the
// world, and the "clientuimodel" ones the HUD's UI models show. A script registers each one by name
// (clientfield::register) and then sets, increments or reads it by that name. The game's builtins look the name up
// (ClientField_Find), and three of them (SetClientField, IncrementClientField, SetUIModelClientField) check whether
// a field came back. Nine more on the server, the four reading ones of the client's scripts (CSC) and the game's two
// value getters read through it at once, and for a name that was never registered that is NULL: the game crashes.
// DEIMOS 935 (zm_ou_deimos) did, about 30 seconds into a game, in GetUIModelClientField.
//
// T7Overcharged, which All-around Enhancement 4 ships, has names for exactly these places (ScriptError_GscClientField-
// Fault1..8, NonExistentClientfield_CrashRip/_CrashFix, ClientFieldFault_Csc1..3, _MissingRip/_MissingRecover and
// _NullRip1/2/_NullRecover, in the table below), but the build AAE 4.1.7 ships has no code that uses them: there are
// the names, addresses for one Windows build and search patterns for some, and nothing reads them
// (wip/night/impl-clientfield.md). So what is done here is what the names and the game's own builtins say:
// - a builtin gets the script error the game's own SetClientField and IncrementClientField throw for a missing
//   field, in the words the lookup itself uses with com_clientFieldsDebug on. The script engine then makes the call's
//   result undefined and the script goes on with its next instruction (the call-builtin opcode's error handler,
//   0x10109558a); QuietScriptErrors shows the error 3 times at each place, then only counts it. T7O's name for
//   SetWorldClientField's place points at that function's end, a silent skip; a setter gives no result, so the script
//   goes on the same way and only gets the error line as well.
// - a value getter, which game code calls too, gets a stand-in field that reads as 0, what T7O's _MissingRecover (the
//   int getter's end, with the NULL still in eax) would give. For the float getter T7O's _NullRecover would give
//   whatever xmm0 held; 0 is given here as well.
// It applies with or without a mod (the user's choice, 2026-11-13): the crash doesn't depend on one, any map's or
// mod's script that asks for a field nobody registered meets it, and the guard only acts where the game would read
// through NULL next. Only to a field asked for with "error if missing": a caller that may do without one checks for
// NULL itself.
#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clientfield_guard.h"
#include "dobby.h"
#include "offsets.h"

#define Set_Count 14         // the game's set names (ADDR_ClientField_SetNames): "world" .. "item"
#define Field_Size 0x140     // a client field, as ClientField_Register clears it
#define Field_Bits 0x130     // int: how many bits it has (the float getter divides by (1 << bits) - 1)
#define Seen_Size 64         // the (place, set, field) keys that got a line in the log
#define Debug_Lines 10       // with BO3MACFIX_CLIENTFIELDDEBUG=1: lines for the first this many times of a key
#define Sets_Pointer 0x8     // in the thread's bgs: the sets, Set_Stride bytes each, as the lookup reads them
#define Set_Stride 0x4018
#define Set_Count_At 0x8     // int: how many fields the set holds
#define Set_Fields 0x10      // ClientField *[Set_Most]
#define Set_Most 0x800       // what fits between Set_Fields and the set's bit count at +0x4010
#define Field_Name 0x120     // scr_string_t
#define Fields_Line_Size 0x4000
#define Message_Size 0x200

static const clientfield_site_t sites[] = {
    // the server's builtins (their table is built at 0x1299bbd), each matched to its Windows function by a text both
    // use
    { ADDR_GScr_CodeGetClientField_Read, Clientfield_Script_Error, 0, "GetClientField",
      "ScriptError_GscClientFieldFault1" },
    { ADDR_GScr_CodeGetPlayerStateClientField_Read, Clientfield_Script_Error, 0, "GetPlayerStateClientField",
      "ScriptError_GscClientFieldFault2" },
    { ADDR_GScr_CodeGetUIModelClientField_Read, Clientfield_Script_Error, 0, "GetUIModelClientField",
      "ScriptError_GscClientFieldFault3" },
    { ADDR_GScr_CodeGetWorldClientField_Read, Clientfield_Script_Error, 0, "GetWorldClientField",
      "ScriptError_GscClientFieldFault4" },
    { ADDR_GScr_CodeIncrementPlayerStateClientField_Read, Clientfield_Script_Error, 0,
      "IncrementPlayerStateClientField", "ScriptError_GscClientFieldFault5" },
    { ADDR_GScr_CodeIncrementUIModelClientField_Read, Clientfield_Script_Error, 0, "IncrementUIModelClientField",
      "ScriptError_GscClientFieldFault6" },
    { ADDR_GScr_CodeIncrementWorldClientField_Read, Clientfield_Script_Error, 0, "IncrementWorldClientField",
      "ScriptError_GscClientFieldFault7" },
    { ADDR_GScr_CodeSetPlayerStateClientField_Read, Clientfield_Script_Error, 0, "SetPlayerStateClientField",
      "ScriptError_GscClientFieldFault8" },
    { ADDR_GScr_CodeSetWorldClientField_Read, Clientfield_Script_Error, 0, "SetWorldClientField",
      "NonExistentClientfield_CrashRip" },
    // the client's builtins (table at 0x100f0ad05). GetClientField has the same fault but no T7O name; it is guarded
    // all the same, the one place here that goes past T7O's list
    { ADDR_CScr_CodeGetClientField_Read, Clientfield_Script_Error, 1, "GetClientField (client)", NULL },
    { ADDR_CScr_CodeGetPlayerStateClientField_Read, Clientfield_Script_Error, 1, "GetPlayerStateClientField (client)",
      "ClientFieldFault_Csc1" },
    { ADDR_CScr_CodeGetUIModelClientField_Read, Clientfield_Script_Error, 1, "GetUIModelClientField (client)",
      "ClientFieldFault_Csc2" },
    { ADDR_CScr_CodeGetWorldClientField_Read, Clientfield_Script_Error, 1, "GetWorldClientField (client)",
      "ClientFieldFault_Csc3" },
    // the value getters, called by the builtins above once they have a field, and by game code
    { ADDR_ClientField_GetInt_Read, Clientfield_Zero, 0, "the int value getter", "ClientFieldFault_MissingRip" },
    { ADDR_ClientField_GetFloat_Read, Clientfield_Zero, 0, "the float value getter", "ClientFieldFault_NullRip1" },
    { ADDR_ClientField_GetFloat_ReadBits, Clientfield_Zero, 0, "the float value getter", "ClientFieldFault_NullRip2" },
};
#define Site_Count (sizeof(sites) / sizeof(sites[0]))

static uint64_t base_address;
static bool debug;
static const char *const *set_names;
static const char *(*SL_ConvertToString)(uint32_t name);
static void (*Scr_Error)(uint32_t inst, const char *message, bool terminal);
static uint8_t **(*BG_ThreadBgs)(void); // for the debug line only

// what the value getters read for a field that isn't there: at offset 0 of the values, mask 0, shift 0, so 0, and 1
// bit, so that the float getter's 1 / ((1 << bits) - 1) is 1 and not infinite (0 * infinity would be NaN)
static uint8_t standin_field[Field_Size] = { [Field_Bits] = 1 };

static os_unfair_lock seen_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t seen[Seen_Size];
static uint32_t seen_times[Seen_Size];
static bool seen_full; // the line that says so has been printed

const clientfield_site_t *clientfield_guard_site(uint64_t return_offset) {
    for (size_t i = 0; i < Site_Count; i++)
        if (sites[i].return_offset == return_offset)
            return &sites[i];
    return NULL;
}

clientfield_action_t clientfield_guard_action(const clientfield_site_t *site, bool found, bool error_if_missing) {
    if (found || !error_if_missing || site == NULL)
        return Clientfield_Pass;
    return (clientfield_action_t)site->kind;
}

const char *clientfield_guard_set_name(const char *const *names, int count, int set) {
    if (names == NULL || set < 0 || set >= count || names[set] == NULL)
        return "unknown";
    return names[set];
}

bool clientfield_guard_first_time(uint64_t *seen_keys, uint32_t *counts, size_t count, uint64_t key, uint32_t *times) {
    key = key != 0 ? key : 1; // 0 marks a free slot
    for (size_t i = 0; i < count; i++) {
        if (seen_keys[i] == key) {
            *times = ++counts[i];
            return false;
        }
        if (seen_keys[i] == 0) {
            seen_keys[i] = key;
            *times = counts[i] = 1;
            return true;
        }
    }
    *times = 0; // more keys than room: no more lines
    return false;
}

void clientfield_guard_set_fields(const uint8_t *set, const char *(*name_of)(uint32_t), char *out, size_t size) {
    int32_t count;
    memcpy(&count, set + Set_Count_At, sizeof(count));
    int32_t shown = count > Set_Most ? Set_Most : count; // a negative count lists nothing
    size_t used = (size_t)snprintf(out, size, "%d fields:", count); // nothing written for size 0
    for (int32_t i = 0; i < shown && used < size; i++) {
        const uint8_t *field;
        memcpy(&field, set + Set_Fields + (size_t)i * sizeof(field), sizeof(field));
        const char *name = NULL;
        if (field != NULL) {
            uint32_t id;
            memcpy(&id, field + Field_Name, sizeof(id));
            name = name_of(id);
        }
        used += (size_t)snprintf(out + used, size - used, "%s%s", i == 0 ? " " : ", ", name != NULL ? name : "?");
    }
    // cut short: say so at the end
    if (used >= size && size > 4)
        memcpy(out + size - 4, "...", 4);
}

uint64_t clientfield_guard_key(size_t place, int set, const char *field) {
    // FNV-1a, as QuietScriptErrors keys its messages
    uint64_t hash = (0xcbf29ce484222325ULL ^ (uint64_t)place) * 0x100000001b3ULL;
    hash = (hash ^ (uint32_t)set) * 0x100000001b3ULL;
    for (; *field != '\0'; field++)
        hash = (hash ^ (uint8_t)*field) * 0x100000001b3ULL;
    return hash;
}

// a line in log.txt the first time each place meets each missing field (with BO3MACFIX_CLIENTFIELDDEBUG=1 also up to
// the 10th time and then each power of two, with T7O's name for the place). A field is told by its text, key as
// clientfield_guard_key makes it, not by its script string id: a script that builds the name anew gets a new id for
// the same text whenever the old string has gone (DEIMOS 935's HUD appends ".time" to a power-up's name each time it
// asks: sixteen different ids for one name in two minutes). Once Seen_Size different ones have been named, one line
// says so and no other gets a line: a table that let the oldest go would name the same fields again and again whenever
// a script asks for more of them in turn than it holds.
static void log_guarded(const clientfield_site_t *site, int set, const char *field, uint64_t key) {
    uint32_t times;
    os_unfair_lock_lock(&seen_lock);
    bool first = clientfield_guard_first_time(seen, seen_times, Seen_Size, key, &times);
    bool say_full = times == 0 && !seen_full;
    seen_full = seen_full || times == 0;
    os_unfair_lock_unlock(&seen_lock);
    if (say_full)
        printf("clientfield: %d different missing fields have been named here; any further one is guarded the same "
               "way, without a line\n", Seen_Size);
    if (!first && !(debug && times != 0 && (times <= Debug_Lines || (times & (times - 1)) == 0)))
        return;
    const char *set_name = clientfield_guard_set_name(set_names, Set_Count, set);
    if (site->kind == Clientfield_Script_Error)
        printf("clientfield: %s asked for '%s' in set '%s', which isn't registered: a script error instead of a "
               "crash", site->function, field, set_name);
    else
        printf("clientfield: %s asked for '%s' in set '%s', which isn't registered: it reads as 0 instead of "
               "crashing", site->function, field, set_name);
    if (debug)
        printf(" (T7Overcharged's %s, game 0x%x, %u times)", site->t7o_name != NULL ? site->t7o_name : "-",
               site->return_offset, times);
    printf("\n");
    // and what the set holds instead, the first time: on the thread that looked it up, as the lookup walked it
    if (!debug || !first || set < 0 || set >= Set_Count || BG_ThreadBgs == NULL)
        return;
    uint8_t **bgs = BG_ThreadBgs();
    const uint8_t *sets = NULL;
    if (bgs != NULL && *bgs != NULL)
        memcpy(&sets, *bgs + Sets_Pointer, sizeof(sets));
    char *line = sets != NULL ? malloc(Fields_Line_Size) : NULL;
    if (line == NULL)
        return;
    clientfield_guard_set_fields(sets + (size_t)set * Set_Stride, SL_ConvertToString, line, Fields_Line_Size);
    printf("clientfield: set '%s' holds %s\n", set_name, line);
    free(line);
}

// the lookup after the game's, with where it was called from; NULL or a field, or no return at all (a script error)
static void *guarded_find(uint64_t set, uint64_t name, uint64_t error_if_missing, uint64_t return_address,
                          void *found) {
    const clientfield_site_t *site = clientfield_guard_site(return_address - base_address);
    // what the game's lookup tests: the set as an int, the name as a script string, "error if missing" as a byte
    clientfield_action_t action = clientfield_guard_action(site, found != NULL, (uint8_t)error_if_missing != 0);
    if (action == Clientfield_Pass)
        return found;
    const char *field = SL_ConvertToString((uint32_t)name);
    field = field != NULL ? field : "?";
    log_guarded(site, (int)set, field, clientfield_guard_key((size_t)(site - sites), (int)set, field));
    if (action == Clientfield_Zero)
        return standin_field;
    char message[Message_Size];
    snprintf(message, sizeof(message), "No ClientField named %s found in set %s", field,
             clientfield_guard_set_name(set_names, Set_Count, (int)set));
    // throws to the script engine, which ends the builtin call with an undefined result. It only comes back while
    // developer_script is on, a mode in which the game's builtins go on after their own script errors too; the
    // caller then gets the game's NULL, as without the guard
    Scr_Error(site->script_instance, message, false);
    return NULL;
}

// ClientField_Find(set, name, error_if_missing)
install_hook_name(ClientField_Find, void *, uint64_t set, uint64_t name, uint64_t error_if_missing) {
    void *found = orig_ClientField_Find(set, name, error_if_missing);
    if (found != NULL)
        return found;
    return guarded_find(set, name, error_if_missing, (uint64_t)__builtin_return_address(0), found);
}

void clientfield_guard_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_CLIENTFIELDDEBUG");
    debug = env != NULL && atoi(env) != 0;
    set_names = (const char *const *)(base_address + ADDR_ClientField_SetNames);
    SL_ConvertToString = (void *)(base_address + ADDR_SL_ConvertToString);
    Scr_Error = (void *)(base_address + ADDR_Scr_Error);
    BG_ThreadBgs = (void *)(base_address + ADDR_BG_ThreadBgs);
    install_hook_ClientField_Find((void *)(base_address + ADDR_ClientField_Find));
    if (debug)
        printf("clientfield: guarding %zu places that read a client field without checking it\n", Site_Count);
}
