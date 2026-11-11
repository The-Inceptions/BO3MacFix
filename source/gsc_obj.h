// Compiled scripts (the game's .gsc assets) and All-around Enhancement 4's patch scripts. Plain C, tested by
// tests/test_gsc_obj.c; gsc_detours.c is the half that talks to the game.
//
// Up to 3.9.5 All-around Enhancement replaced whole stock scripts (scripts/zm/_zm.gsc ...) with its own. Since 4.0 it
// leaves them alone and ships patch scripts instead: scripts/aae_common/zm/_zm.gsc for scripts/zm/_zm.gsc on every
// map, scripts/aae_level/<map>/zm/<name>.gsc for one map. A patch script is a normal compiled script behind a "GSIC"
// header that lists detours: "this function of mine replaces that function of that script". Its Windows plugin loads
// each one after the script it patches and sends every call of a replaced function to the replacement, except the
// calls the replacement makes itself, which is how it runs the original. Without that the game never loads the
// patch scripts, and what moved into them (the starting round, Perkaholic, the zombie counter ...) does nothing.
//
// The compiled script format, as far as this needs it (little endian, offsets from the start of the script):
//   0x00 magic            0x0c include table     0x14 code           0x20 export table   0x24 import table
//   0x30 code size        0x34 name              0x3a export count   0x3c import count   0x44 include count (byte)
//   0x46 flags (byte)
//   include table: an offset to the text per include ("scripts/zm/_zm", no extension)
//   export (20 bytes): checksum, code offset, name hash, namespace hash, parameter count (byte), flags (byte)
//   import (12 bytes + 4 per call): name hash, namespace hash, call count (16 bits), parameter count (byte), flags
//       (byte), then the offset of each call's opcode. The game's linker writes the function's address behind it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define Gsc_Magic 0x1c000a0d43534780ULL
#define Gsc_Include_Offset 0x0c
#define Gsc_Code_Offset 0x14
#define Gsc_Exports_Offset 0x20
#define Gsc_Imports_Offset 0x24
#define Gsc_Code_Size 0x30
#define Gsc_Name_Offset 0x34
#define Gsc_Exports_Count 0x3a
#define Gsc_Imports_Count 0x3c
#define Gsc_Include_Count 0x44
#define Gsc_Flags 0x46
#define Gsc_Header_Size 0x48
#define Gsc_Export_Size 20
#define Gsc_Import_Size 12

// a script's flags
#define Gsc_Flag_Restricted 0x02 // its opcodes come from the second set, and some builtins are closed to it
// an import's flags
#define Gsc_Import_Type 0x0f // one of the Gsc_Call_ values
#define Gsc_Import_Dev 0x10  // written inside a /# #/ block
#define Gsc_Import_Any 0x20  // written without a namespace: this script's, an include's, or a builtin
#define Gsc_Call_Address 1   // &f
#define Gsc_Call_Function 2  // f()
#define Gsc_Call_Thread 3    // thread f()
#define Gsc_Call_Method 4    // e f()
#define Gsc_Call_Method_Thread 5 // e thread f()
// an export's flags
#define Gsc_Export_Private 0x04
#define Gsc_Export_Varargs 0x20

typedef struct {
    uint32_t address, name, name_space;
    uint8_t params, flags;
} gsc_export_t;

typedef struct {
    uint32_t name, name_space;
    uint16_t call_count;
    uint8_t params, flags;
    const uint8_t *calls; // call_count unaligned 32-bit offsets
} gsc_import_t;

// the game's hash of a function, namespace or dvar name
uint32_t gsc_hash(const char *name);

// whether size bytes at obj are a compiled script whose name, includes, exports, code and imports lie inside them
bool gsc_obj_check(const uint8_t *obj, size_t size);
// The rest takes a script that passed gsc_obj_check, or one the game has loaded (which it trusts the same way).
const char *gsc_obj_name(const uint8_t *obj); // "scripts/zm/_zm.gsc"; "" for one compiled without a name
unsigned gsc_obj_include_count(const uint8_t *obj);
const char *gsc_obj_include(const uint8_t *obj, unsigned index);
unsigned gsc_obj_export_count(const uint8_t *obj);
void gsc_obj_export(const uint8_t *obj, unsigned index, gsc_export_t *out);
bool gsc_obj_find_export(const uint8_t *obj, uint32_t name_space, uint32_t name, gsc_export_t *out);
// walks the imports: start with *cursor = 0 and call until it returns false
bool gsc_obj_next_import(const uint8_t *obj, unsigned *index, size_t *cursor, gsc_import_t *out);
uint32_t gsc_import_call(const gsc_import_t *import, unsigned index);
// where the linker puts the function's address for an import's call at that offset
static inline uint32_t gsc_call_slot(uint32_t call, uint8_t import_flags) {
    return (call + ((import_flags & Gsc_Import_Type) == Gsc_Call_Address ? 9 : 10)) & ~7u;
}

#pragma mark - includes

// All-around Enhancement's compiler leaves "#using" in its patch scripts' include tables, which the game would look
// for as a script and stop the map load over ("#include" and "#using_animtree" count the same on Windows).
bool gsc_include_is_directive(const char *text);
typedef struct {
    uint8_t count;
    bool changed;
    uint32_t entries[255];
} gsc_include_backup_t;
// takes those entries out of the include table (the rest move up, the count shrinks), returns whether it had any
bool gsc_includes_strip(uint8_t *obj, gsc_include_backup_t *backup);
void gsc_includes_restore(uint8_t *obj, const gsc_include_backup_t *backup);

#pragma mark - whether the game would link it

// The game's linker stops the map load ("Unresolved external ...") when a script calls something it can't find, so a
// patch script is only handed to it when this says every import resolves, by the linker's own rules.
typedef struct {
    // the compiled script an include names ("scripts/zm/_zm"), NULL when the game has none
    const uint8_t *(*script)(void *context, const char *include);
    // a builtin function or method by name hash: its kind (0 any script, 1 only inside /# #/, 2 not for restricted
    // scripts) and how many parameters it takes; false when the game has none
    bool (*builtin)(void *context, uint32_t name, bool method, int *kind, int *min_params, int *max_params);
    void *context;
    bool skips_dev_calls; // the linker leaves an unrestricted script's /# #/ calls alone (the game's own check)
} gsc_link_env_t;
// false with the reason in why (an include the game lacks, a function nothing has ...)
bool gsc_obj_links(const uint8_t *obj, const gsc_link_env_t *env, char *why, size_t why_size);

#pragma mark - patch scripts

// A patch script asset: "GSIC", the number of sections (1), then per section a script instance (0, the server's),
// a count and that many 0x100-byte detours { replacement's name hash, target's namespace hash, target's name hash,
// replacement's code offset, its size, target script's name (236 bytes) }, then the compiled script.
#define Gsic_Magic 0x43495347u
#define Gsic_Entry_Size 0x100
#define Gsic_Path_Offset 0x14
#define Gsic_Max_Sections 8
#define Gsic_Max_Detours 0x400
#define Gsic_Max_Path 0x9f // characters

typedef struct {
    const uint8_t *body; // the compiled script
    size_t body_size;
    uint32_t count; // detours, over all sections
    uint32_t sections;
    const uint8_t *entries[Gsic_Max_Sections];
    uint32_t counts[Gsic_Max_Sections];
} gsic_t;

typedef struct {
    uint32_t replacement, target_name_space, target_name, offset, size;
    const char *target_script; // "scripts/zm/_zm.gsc"
} gsic_detour_t;

// whether the asset starts like a patch script
bool gsic_is(const uint8_t *asset, size_t size);
// reads its header, false unless every detour is sound (a named target, code inside the script) and the script
// passes gsc_obj_check; the same things its Windows plugin refuses
bool gsic_parse(const uint8_t *asset, size_t size, gsic_t *out);
void gsic_detour(const gsic_t *gsic, uint32_t index, gsic_detour_t *out);

// Which patch script goes with a loaded script: "scripts/zm/_zm.gsc" gives "scripts/aae_common/zm/_zm", or with a
// map "scripts/aae_level/<map>/zm/_zm" (the name Scr_LoadScript takes, the asset's adds ".gsc"). False for a script
// that can't have one: not under scripts/, not a .gsc, or itself one of All-around Enhancement's.
bool gsc_mirror_name(const char *script, const char *map, char *out, size_t out_size);
// the maps patch scripts are for: zm_ and lowercase letters, digits and '_', 3 to 63 characters
bool gsc_map_name_valid(const char *map);

#pragma mark - detours

typedef struct {
    const uint8_t *mirror; // the loaded patch script
    uint32_t offset, size; // the replacement's code in it
    uint32_t target_name_space, target_name;
    char target_script[Gsic_Max_Path + 1];
    const uint8_t *target; // the function it replaces, NULL until its script is loaded
} gsc_rule_t;

typedef struct {
    gsc_rule_t rules[Gsic_Max_Detours];
    uint32_t count;
    uint16_t order[Gsic_Max_Detours]; // the rules with a target, by target and then by age
    uint32_t resolved;
} gsc_rules_t;

void gsc_rules_clear(gsc_rules_t *rules);
// adds a loaded patch script's detours, false (and none added) when they don't all fit
bool gsc_rules_add(gsc_rules_t *rules, const gsic_t *gsic);
// finds the functions of the rules still waiting for their script among the loaded scripts, returns how many it found
uint32_t gsc_rules_resolve(gsc_rules_t *rules, const uint8_t *const *scripts, uint32_t script_count);
// In a linked script, sends every call of a replaced function to its replacement: the address behind the call's
// opcode changes. The calls inside a replacement of that same function stay, so it can run the original. With two
// replacements of one function the first added wins. Returns the calls changed; a second pass changes nothing.
uint32_t gsc_rules_patch(const gsc_rules_t *rules, uint8_t *obj);
