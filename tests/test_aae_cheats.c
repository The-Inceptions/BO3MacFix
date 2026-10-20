// aae_cheats.c: when a map start keeps sv_cheats off, and its reads of the console command's name, the loaded mod's
// name and elmg_cheats, against made-up memory laid out like the game's
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "../source/aae_cheats.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// the game's side: hooking, and the dvar functions
static void *hooked_address = NULL, *hooked_fake = NULL;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    hooked_address = address;
    hooked_fake = fake_func;
    *out_origin_func = NULL;
    return 0;
}

static int fake_elmg_dvar = 0, elmg_value = 0;
static bool elmg_exists = false;
static uint32_t asked_hash = 0;
static void *find_by_hash(uint32_t hash) {
    asked_hash = hash;
    return elmg_exists ? &fake_elmg_dvar : NULL;
}
static int get_int(void *dvar) {
    return dvar == &fake_elmg_dvar ? elmg_value : -1;
}

static int set_calls = 0;
static bool set_value = false;
static void *set_dvar = NULL;
static void record_set_bool(void *dvar, bool value) {
    set_calls++;
    set_dvar = dvar;
    set_value = value;
}

static void test_decision(void) {
    CHECK(aae_cheats_keep_off("map", Aae_Mod_Name, 0));
    CHECK(aae_cheats_keep_off("MAP", Aae_Mod_Name, 0));
    // devmap, whatever its case, keeps cheats on
    CHECK(!aae_cheats_keep_off("devmap", Aae_Mod_Name, 0));
    CHECK(!aae_cheats_keep_off("DevMap", Aae_Mod_Name, 0));
    // AAE's own cheat switch
    CHECK(!aae_cheats_keep_off("map", Aae_Mod_Name, 1));
    CHECK(!aae_cheats_keep_off("map", Aae_Mod_Name, -1));
    // other mods, and none (the map command doesn't set it then anyway)
    CHECK(!aae_cheats_keep_off("map", "1234567890", 0));
    CHECK(!aae_cheats_keep_off("map", "26319431230", 0));
    CHECK(!aae_cheats_keep_off("map", "", 0));
    // what couldn't be read keeps the game's behaviour
    CHECK(!aae_cheats_keep_off(NULL, Aae_Mod_Name, 0));
    CHECK(!aae_cheats_keep_off("map", NULL, 0));
}

// memory from the mod's name to past the command's arguments, at the game's offsets from base
static uint8_t *memory = NULL;
static const uint64_t memory_start = ADDR_Mods_LoadedName & ~0xFFFULL, memory_end = ADDR_cmd_args + 0x1000;

static void set_command(int nesting, const char *const *argv, int argc) {
    uint8_t *args = memory + (ADDR_cmd_args - memory_start);
    memset(args, 0, 0x100);
    memcpy(args, &nesting, sizeof(nesting));
    memcpy(args + 0x64 + 4 * nesting, &argc, sizeof(argc));
    memcpy(args + 0x88 + 8 * nesting, &argv, sizeof(argv));
}

static void set_mod(const char *name) {
    char *mod = (char *)memory + (ADDR_Mods_LoadedName - memory_start);
    memset(mod, 0, 0x20);
    strncpy(mod, name, 0x20); // a full 0x20 bytes has no NUL in the game's buffer either
}

static void test_reads(void) {
    memory = mmap(NULL, memory_end - memory_start, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(memory != MAP_FAILED);
    if (memory == MAP_FAILED)
        return;
    base = (uint64_t)memory - memory_start;
    Dvar_FindByHash = find_by_hash;
    Dvar_GetInt = get_int;

    static const char *const map[] = { "map", "zm_theater" }, *const devmap[] = { "devmap", "zm_theater" };
    set_mod(Aae_Mod_Name);
    set_command(0, map, 2);
    CHECK(keep_cheats_off());
    CHECK(asked_hash == loadout_script_hash("elmg_cheats"));
    // a command run from inside another one (exec, a bind): its own arguments
    set_command(2, map, 2);
    CHECK(keep_cheats_off());
    set_command(1, devmap, 2);
    CHECK(!keep_cheats_off());

    set_command(0, map, 2);
    elmg_exists = true;
    elmg_value = 1;
    CHECK(!keep_cheats_off());
    elmg_value = 0;
    CHECK(keep_cheats_off());
    elmg_exists = false;

    set_mod("1234567890");
    CHECK(!keep_cheats_off());
    set_mod("");
    CHECK(!keep_cheats_off());
    set_mod("0123456789abcdef0123456789abcdef"); // all 0x20 bytes
    CHECK(!keep_cheats_off());
    set_mod(Aae_Mod_Name);

    // arguments that can't be read: no command, a nesting out of range, an argv nothing is mapped at
    set_command(0, map, 0);
    CHECK(!keep_cheats_off());
    set_command(0, map, 2);
    int32_t bad_nesting = 8;
    memcpy(memory + (ADDR_cmd_args - memory_start), &bad_nesting, sizeof(bad_nesting));
    CHECK(!keep_cheats_off());
    set_command(0, (const char *const *)(uintptr_t)0x10, 2);
    CHECK(!keep_cheats_off());
    set_command(0, map, 2);
    CHECK(keep_cheats_off());
}

// the hook leaves every other Dvar_SetBool alone (these calls don't return into the map command)
static void test_hook(void) {
    aae_cheats_install(0, true);
    CHECK(hooked_fake == NULL);
    uint64_t test_base = base;
    aae_cheats_install(test_base, false);
    CHECK(hooked_fake == NULL);
    aae_cheats_install(test_base, true);
    CHECK(hooked_address == (void *)(test_base + ADDR_Dvar_SetBool));
    CHECK(hooked_fake == (void *)fake_Dvar_SetBool);
    Dvar_FindByHash = find_by_hash;
    Dvar_GetInt = get_int;
    orig_Dvar_SetBool = record_set_bool;
    int dvar = 0;
    fake_Dvar_SetBool(&dvar, true);
    CHECK(set_calls == 1 && set_dvar == &dvar && set_value);
    fake_Dvar_SetBool(&dvar, false);
    CHECK(set_calls == 2 && !set_value);
}

int main(void) {
    test_decision();
    test_reads();
    test_hook();
    if (failures == 0)
        printf("test_aae_cheats: all passed\n");
    return failures == 0 ? 0 : 1;
}
