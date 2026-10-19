// Tests zm_inventory.c's pass through the game's cheat check against a made-up g_entities: only the give and take
// commands get it, for the local player while alive, as many as were allowed and only for a while. Every other cheat
// command, and any other caller, gets the game's own answer.
// Then the full loadout against a made-up game image (offsets.h's addresses in one mapped region), with stand-ins for
// the game's functions and a made-up script VM (fake_vm.h): reading the guns, ammo types and perks, and "give
// bo3macfix_restore" giving them back the way the Pack-a-Punch machine does, in the right order and only when set up.
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "../source/zm_inventory.c"
#include "fake_vm.h"

// what zm_inventory.c links against; the hook is called directly here
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    return -1;
}

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAILED line %d: %s\n", __LINE__, #cond);                                                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Entity_Size 0x4f8
static uint64_t entities_address; // what the game's pointer at ADDR_g_entities_Ptr holds
_Alignas(8) static uint8_t g_entities[3 * Entity_Size];
static int game_checks;

// the game's own CheatsOk, with sv_cheats off
static int game_cheats_ok(void *ent) {
    game_checks++;
    return 0;
}

static void set_health(int entity, int health) {
    memcpy(g_entities + entity * Entity_Size + gentity_Health, &health, sizeof(health));
}

// ---- the full loadout, against a made-up game image

#define Image_Size 0x15a00000 // past every address the loadout uses (ADDR_scrVarGlob is the last)
static uint8_t *image;
static fake_vm_t vm;
static uint8_t vm_memory[FakeVm_Size];
static uint32_t vm_player, vm_aats, vm_perks;
_Alignas(8) static uint8_t player_entity[Entity_Size], other_entity[Entity_Size];
_Alignas(8) static uint8_t client[0x16c00];
static char calls[4096];

static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *format, ...) {
    size_t used = strlen(calls);
    va_list args;
    va_start(args, format);
    vsnprintf(calls + used, sizeof(calls) - used, format, args);
    va_end(args);
    used = strlen(calls);
    snprintf(calls + used, sizeof(calls) - used, "; ");
}

static void image_put(uint64_t offset, const void *value, size_t size) {
    memcpy(image + offset, value, size);
}

// the weapons this game has: index, name, inventory type
static const struct {
    uint32_t index;
    const char *name;
    int type;
} weapons[] = { { 4, "pistol_standard_zm", 0 },       { 7, "frag_grenade_zm", 1 },
                { 0x23, "ar_standard_upgraded_zm", 0 }, { 0xa5, "ray_gun_upgraded_zm", 0 } };
#define Weapon_Count (sizeof(weapons) / sizeof(weapons[0]))
_Alignas(8) static uint8_t variants[Weapon_Count][0x20], defs[Weapon_Count][0x80];
static const uint64_t pistol = 4, frag = 7, ar_bare = 0x23, raygun = 0xa5;
static const uint64_t ar = 0x23 | (33ULL << 9) | (8ULL << 15); // +reflex+extclip
static const char *const attachment_table[44] = { "none", "acog", [8] = "extclip", [11] = "fmj", [33] = "reflex" };
static int code_acquire, code_give_perk, code_build_kit, code_default;
static bool acquire_missing; // the scripts have no aat::acquire (a mod without AATs)
static const void *default_code = &code_default;

// the held weapons' table
static uint64_t *held_slot(int i) { return (uint64_t *)(client + gclient_HeldWeapons + i * gclient_HeldWeapon_Size); }
static void hold(int i, uint64_t weapon, uint64_t options, uint32_t acvi) {
    *held_slot(i) = weapon;
    memcpy(client + gclient_HeldWeapons + i * gclient_HeldWeapon_Size + gclient_HeldWeapon_Options, &options, 8);
    memcpy(client + gclient_HeldWeapons + i * gclient_HeldWeapon_Size + gclient_HeldWeapon_Acvi, &acvi, 4);
}
static uint64_t in_hand(void) { return *(uint64_t *)(client + gclient_Weapon); }
static void set_in_hand(uint64_t weapon) { *(uint64_t *)(client + gclient_Weapon) = weapon; }

static char perks_had[8][32];
static int perk_count;
static const char *const perk_names[] = { "specialty_armorvest", "specialty_quickrevive", "specialty_doubletap2",
                                          "specialty_fastreload" };

// stand-ins for the game's functions
static int fake_give(void *ent, void *who, uint64_t weapon, uint64_t options, int unused, int acvi) {
    note("give %llx %llx %d", (unsigned long long)weapon, (unsigned long long)options, acvi);
    for (int i = 0; i < gclient_HeldWeapon_Count; i++) {
        if (*held_slot(i) == 0) {
            hold(i, weapon, options, (uint32_t)acvi);
            return 1;
        }
    }
    return 0;
}
static void fake_start_ammo(void *ent, uint64_t weapon, int held_before) {
    note("ammo %llx", (unsigned long long)weapon);
}
static int fake_take(void *ent, void *who, uint64_t weapon, bool flag) {
    note("take %llx%s", (unsigned long long)weapon, in_hand() == 0 ? " (hands empty)" : "");
    for (int i = 0; i < gclient_HeldWeapon_Count; i++)
        if (*held_slot(i) == weapon)
            hold(i, 0, 0, 0);
    return 1;
}
static void fake_select(int client_num, uint64_t weapon) {
    note("select %d %llx", client_num, (unsigned long long)weapon);
}
static void fake_switch(void *ent, int immediate, uint64_t weapon) {
    note("switch %llx", (unsigned long long)weapon);
    set_in_hand(weapon);
}
static uint64_t fake_weapon_for_name(const char *name) {
    if (strcmp(name, "ar_standard_upgraded_zm+reflex+extclip") == 0)
        return ar;
    for (size_t i = 0; i < Weapon_Count; i++)
        if (strcmp(name, weapons[i].name) == 0)
            return weapons[i].index;
    return 0; // the game's "none"
}
static int fake_perk_index(const char *name) {
    for (int i = 0; i < 4; i++)
        if (strcmp(name, perk_names[i]) == 0)
            return 10 + i;
    return Perk_None;
}
static bool fake_has_perk(void *who, int index) {
    for (int i = 0; i < perk_count; i++)
        if (fake_perk_index(perks_had[i]) == index)
            return true;
    return false;
}
static const char *argv1 = "bo3macfix_restore";
static void fake_argv(int arg, char *buffer, int size) {
    snprintf(buffer, (size_t)size, "%s", arg == 0 ? "give" : arg == 1 ? argv1 : "");
}
static script_handle_t *fake_function_handle(script_handle_t *out, int inst, const char *script, uint32_t ns,
                                             uint32_t name, int *checksum, bool required) {
    out->code_pos = default_code;
    if (inst != Script_Server || checksum != NULL || required)
        return out;
    if (strcmp(script, "scripts/shared/aat_shared") == 0 && ns == loadout_script_hash("aat") &&
        name == loadout_script_hash("acquire") && !acquire_missing)
        out->code_pos = &code_acquire;
    if (strcmp(script, "scripts/zm/_zm_perks") == 0 && ns == loadout_script_hash("zm_perks") &&
        name == loadout_script_hash("give_perk"))
        out->code_pos = &code_give_perk;
    if (strcmp(script, "scripts/zm/_zm_weapons") == 0 && ns == loadout_script_hash("zm_weapons") &&
        name == loadout_script_hash("give_build_kit_weapon"))
        out->code_pos = &code_build_kit;
    snprintf(out->name, sizeof(out->name), "%s::%08x", script, name);
    return out;
}
// the script VM's parameters, the last one pushed on top
static struct {
    int type; // 's' string, 'w' weapon, 'i' int
    uint64_t value;
    char text[48];
} params[8];
static int param_count;
static void fake_add_string(int inst, const char *text) {
    params[param_count].type = 's';
    snprintf(params[param_count++].text, sizeof(params[0].text), "%s", text);
}
static void fake_add_weapon(int inst, uint64_t weapon, int client_num) {
    params[param_count].type = 'w';
    params[param_count++].value = weapon;
}
static void fake_add_int(int inst, int value) {
    params[param_count].type = 'i';
    params[param_count++].value = (uint64_t)value;
}
// runs a "script function": the first parameter is the one pushed last
static unsigned int fake_exec(void *ent, int count, script_handle_t handle) {
    if (count != param_count) {
        note("exec with %d of %d parameters", count, param_count);
        param_count = 0;
        return 0;
    }
#define Param(n) params[param_count - 1 - (n)]
    if (handle.code_pos == &code_acquire && count == 2 && Param(0).type == 'w' && Param(1).type == 's') {
        note("acquire %llx %s", (unsigned long long)Param(0).value, Param(1).text);
        fake_vm_add_aat(&vm, vm_aats, Param(0).value, Param(1).text);
    } else if (handle.code_pos == &code_give_perk && count == 2 && Param(0).type == 's' && Param(1).type == 'i' &&
               Param(1).value == 0) {
        note("give_perk %s", Param(0).text);
        if (fake_perk_index(Param(0).text) != Perk_None)
            snprintf(perks_had[perk_count++], sizeof(perks_had[0]), "%s", Param(0).text);
    } else if (handle.code_pos == &code_build_kit && count == 1 && Param(0).type == 'w') {
        // the build kit's reflex and extclip, and the map's Pack-a-Punch camo
        uint64_t weapon = Param(0).value == ar_bare ? ar : Param(0).value;
        note("build kit %llx", (unsigned long long)Param(0).value);
        fake_give(ent, NULL, weapon, 0x1a, 0, 0);
    } else {
        note("exec %s", handle.name);
    }
#undef Param
    param_count = 0;
    return 77;
}
static void fake_free(int inst, unsigned int thread) {
    if (inst != Script_Server || thread != 77)
        note("free %d %u", inst, thread);
}

static void make_image(void) {
    image = mmap(NULL, Image_Size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (image == MAP_FAILED) {
        printf("FAILED: couldn't map a made-up game image\n");
        exit(1);
    }
    base = (uint64_t)image;
    uint64_t entities_at = (uint64_t)player_entity;
    image_put(ADDR_g_entities_Ptr, &entities_at, 8);
    uint64_t client_at = (uint64_t)client;
    memcpy(player_entity + gentity_Client, &client_at, 8);
    memcpy(other_entity + gentity_Client, &client_at, 8);
    int health = 100, last = 0xff;
    memcpy(player_entity + gentity_Health, &health, 4);
    memcpy(other_entity + gentity_Health, &health, 4);
    // the weapon tables
    image_put(ADDR_bg_lastWeaponIndex, &last, 4);
    for (size_t i = 0; i < Weapon_Count; i++) {
        uint64_t name = (uint64_t)weapons[i].name, def = (uint64_t)defs[i], variant = (uint64_t)variants[i];
        memcpy(variants[i], &name, 8);
        // the name the server looks it up by: the AR has one without "_zm", as zombies' weapons do
        if (weapons[i].index == 0x23) {
            uint64_t script_name = (uint64_t)"ar_standard_upgraded";
            memcpy(variants[i] + WeaponVariantDef_ScriptName, &script_name, 8);
        }
        memcpy(variants[i] + WeaponVariantDef_WeaponDef, &def, 8);
        memcpy(defs[i] + WeaponDef_InventoryType, &weapons[i].type, 4);
        image_put(ADDR_bg_weaponVariantDefs + weapons[i].index * 8, &variant, 8);
    }
    image_put(ADDR_bg_attachmentNames, attachment_table, sizeof(attachment_table));
    image_put(ADDR_scrDefaultFunction, &default_code, 8);
    // the server script VM: its globals, class table and strings where the game has their pointers
    fake_vm_init(&vm, vm_memory, (uint64_t)vm_memory);
    image_put(ADDR_scrVarGlob, vm_memory + FakeVm_Glob, 0x100);
    uint64_t classes = vm.address + FakeVm_Classes, strings = vm.address + FakeVm_String_Table;
    image_put(ADDR_scrClassMap, &classes, 8);
    image_put(ADDR_scrStringTable, &strings, 8);
    vm_player = fake_vm_player(&vm, 0);
    vm_aats = fake_vm_array_field(&vm, vm_player, "aat");
    vm_perks = fake_vm_array_field(&vm, vm_player, "perks_active");
    // the game's functions
    G_GivePlayerWeapon = fake_give;
    G_GiveStartAmmo = fake_start_ammo;
    G_TakePlayerWeapon = fake_take;
    G_SelectWeapon = fake_select;
    G_SwitchToWeapon = fake_switch;
    BG_GetWeaponForName = fake_weapon_for_name;
    BG_GetPerkIndexForName = fake_perk_index;
    BG_HasPerk = fake_has_perk;
    SV_Cmd_ArgvBuffer = fake_argv;
    Scr_GetFunctionHandle = fake_function_handle;
    Scr_ExecEntThread = fake_exec;
    Scr_FreeThread = fake_free;
    Scr_AddString = fake_add_string;
    Scr_AddWeapon = fake_add_weapon;
    Scr_AddInt = fake_add_int;
}

// the restore's give command, run now (as the give command's cheat check would run it); its result line
static const char *restore_now(void *ent) {
    static char result[512];
    calls[0] = '\0';
    run_restore(ent);
    if (!zm_inventory_restore_result(result, sizeof(result)))
        snprintf(result, sizeof(result), "(no result)");
    return result;
}

static void test_loadout(void) {
    make_image();
    // the saved game: the starting pistol, a Pack-a-Punched AR with attachments, the ray gun in hand, a grenade
    hold(0, pistol, 0, 0);
    hold(1, ar, 0x822a, 17);
    hold(2, raygun, 0x2a, 0);
    hold(3, frag, 0, 0);
    set_in_hand(raygun);
    fake_vm_add_aat(&vm, vm_aats, ar, "zm_aat_dead_wire");
    fake_vm_add_aat(&vm, vm_aats, raygun, "zm_aat_blast_furnace");
    fake_vm_add_string(&vm, vm_perks, 0, "specialty_armorvest");
    fake_vm_add_string(&vm, vm_perks, 1, "specialty_doubletap2");

    loadout_t loadout;
    char note_text[256];
    CHECK(zm_inventory_read_loadout(&loadout, note_text, sizeof(note_text)));
    CHECK(note_text[0] == '\0');
    CHECK(loadout.gun_count == 3 && loadout.held == 2 && loadout.perk_count == 2);
    CHECK(strcmp(loadout.guns[0].name, "pistol_standard_zm") == 0 && loadout.guns[0].aat[0] == '\0');
    CHECK(strcmp(loadout.guns[1].name, "ar_standard_upgraded+reflex+extclip") == 0 &&
          loadout.guns[1].options == 0x822a && loadout.guns[1].acvi == 17 &&
          strcmp(loadout.guns[1].aat, "zm_aat_dead_wire") == 0);
    CHECK(strcmp(loadout.guns[2].name, "ray_gun_upgraded_zm") == 0 && loadout.guns[2].options == 0x2a &&
          strcmp(loadout.guns[2].aat, "zm_aat_blast_furnace") == 0);
    CHECK(strcmp(loadout.perks[0], "specialty_armorvest") == 0 && strcmp(loadout.perks[1], "specialty_doubletap2") == 0);
    char text[1024];
    CHECK(loadout_format_guns(&loadout, text, sizeof(text)));
    CHECK(strcmp(text, "pistol_standard_zm:0:0:,ar_standard_upgraded+reflex+extclip:822a:17:zm_aat_dead_wire,"
                       "ray_gun_upgraded_zm:2a:0:zm_aat_blast_furnace") == 0);
    // no script object for the player (another VM state): the guns still, without ammo types or perks, and a note
    uint32_t real_class = fake_vm_get32(&vm, FakeVm_Classes + 4);
    fake_vm_put32(&vm, FakeVm_Classes + 4, 0);
    loadout_t bare;
    CHECK(zm_inventory_read_loadout(&bare, note_text, sizeof(note_text)));
    CHECK(bare.gun_count == 3 && bare.guns[1].aat[0] == '\0' && bare.perk_count == 0);
    CHECK(strstr(note_text, "no script object") != NULL);
    fake_vm_put32(&vm, FakeVm_Classes + 4, real_class);

    // the resumed game: the starting pistol in hand, Quick Revive from the start, no ammo types yet
    memset(client + gclient_HeldWeapons, 0, gclient_HeldWeapon_Count * gclient_HeldWeapon_Size);
    hold(0, pistol, 0, 0);
    hold(1, frag, 0, 0);
    hold(2, 0x99, 0, 0); // AAE's starting gun, not saved... but not a weapon this game knows: left alone
    set_in_hand(pistol);
    fake_vm_init(&vm, vm_memory, (uint64_t)vm_memory);
    image_put(ADDR_scrVarGlob, vm_memory + FakeVm_Glob, 0x100);
    vm_player = fake_vm_player(&vm, 0);
    vm_aats = fake_vm_array_field(&vm, vm_player, "aat");
    vm_perks = fake_vm_array_field(&vm, vm_player, "perks_active");
    snprintf(perks_had[0], sizeof(perks_had[0]), "specialty_quickrevive");
    perk_count = 1;

    // nothing set up: nothing happens, and nothing to tell
    CHECK(strcmp(restore_now(player_entity), "(no result)") == 0 && calls[0] == '\0');
    // the saved game without the pistol, the ray gun in hand, Jugg and Quick Revive
    // (the AR by the name the server has it by; the lookup tries its other form too)
    CHECK(loadout_parse("ar_standard_upgraded+reflex+extclip:822a:17:zm_aat_dead_wire,"
                        "ray_gun_upgraded:2a:0:zm_aat_blast_furnace", "ray_gun_upgraded",
                        "specialty_armorvest,specialty_quickrevive", &loadout));
    CHECK(strcmp(zm_inventory_set_restore(&loadout), "give bo3macfix_restore") == 0);
    // only for the local player while it's alive (and that uses it up)
    CHECK(strcmp(restore_now(other_entity), "not for the local player, or it isn't alive") == 0 && calls[0] == '\0');
    zm_inventory_set_restore(&loadout);
    int dead = 0, alive = 100;
    memcpy(player_entity + gentity_Health, &dead, 4);
    CHECK(strcmp(restore_now(player_entity), "not for the local player, or it isn't alive") == 0);
    memcpy(player_entity + gentity_Health, &alive, 4);
    // too long ago
    zm_inventory_set_restore(&loadout);
    restore_until = now_ms() - 1;
    CHECK(strstr(restore_now(player_entity), "nothing set up") != NULL);

    zm_inventory_set_restore(&loadout);
    const char *result = restore_now(player_entity);
    CHECK(strcmp(calls, "give_perk specialty_armorvest; select 0 0; take 4 (hands empty); give 44223 822a 17; ammo 44223; "
                        "give a5 2a 0; ammo a5; acquire 44223 zm_aat_dead_wire; acquire a5 zm_aat_blast_furnace; "
                        "switch a5; ") == 0);
    if (strcmp(calls, "give_perk specialty_armorvest; select 0 0; take 4 (hands empty); give 44223 822a 17; ammo 44223; "
                      "give a5 2a 0; ammo a5; acquire 44223 zm_aat_dead_wire; acquire a5 zm_aat_blast_furnace; "
                      "switch a5; ") != 0)
        printf("  calls: %s\n", calls);
    CHECK(strcmp(result, "took 1, gave 2 guns (0 held already), 2 ammo types, 1 perks (1 had already), 0 failed") == 0);
    CHECK(!zm_inventory_restore_result(text, sizeof(text))); // told once
    CHECK(*held_slot(0) == ar || *held_slot(0) == raygun);
    CHECK(in_hand() == raygun);
    // once only
    CHECK(strcmp(restore_now(player_entity), "(no result)") == 0 && calls[0] == '\0');
    // the command sent again (the first held back while paused) running after the first: what Lua reads is still the
    // restore's result, not "nothing set up", which would have it set the restore up and restore once more
    zm_inventory_set_restore(&loadout);
    run_restore(player_entity);
    calls[0] = '\0';
    run_restore(player_entity);
    CHECK(calls[0] == '\0');
    CHECK(zm_inventory_restore_result(text, sizeof(text)) && strncmp(text, "took ", 5) == 0);
    CHECK(!zm_inventory_restore_result(text, sizeof(text)));

    // the game without aat_shared's acquire (no AATs here) or without the perk: told, the rest still given
    memset(client + gclient_HeldWeapons, 0, gclient_HeldWeapon_Count * gclient_HeldWeapon_Size);
    hold(0, pistol, 0, 0);
    perk_count = 0;
    CHECK(loadout_parse("ray_gun_upgraded_zm:2a:0:zm_aat_turned", NULL, "specialty_phdflopper", &loadout));
    script_handle_t handle;
    CHECK(script_function(&handle, "scripts/shared/aat_shared", "aat", "acquire") && handle.code_pos == &code_acquire);
    CHECK(!script_function(&handle, "scripts/shared/aat_shared", "aat", "nothing")); // the do-nothing function
    acquire_missing = true;
    zm_inventory_set_restore(&loadout);
    result = restore_now(player_entity);
    acquire_missing = false;
    CHECK(strcmp(calls, "take 4; give a5 2a 0; ammo a5; switch a5; ") == 0);
    CHECK(strcmp(result, "took 1, gave 1 guns (0 held already), 0 ammo types, 0 perks (0 had already), 2 failed: no "
                         "perk specialty_phdflopper, no ammo type zm_aat_turned") == 0);

    // the test switch's: the Pack-a-Punch machine's way, the build kit's attachments, then the ammo type
    memset(client + gclient_HeldWeapons, 0, gclient_HeldWeapon_Count * gclient_HeldWeapon_Size);
    hold(0, pistol, 0, 0);
    set_in_hand(pistol);
    CHECK(loadout_parse("ar_standard_upgraded_zm:0:0:zm_aat_turned,thundergun_zm:0:0:", NULL, "specialty_fastreload",
                        &loadout));
    zm_inventory_set_pap_test(&loadout);
    result = restore_now(player_entity);
    CHECK(strcmp(calls, "build kit 23; give 44223 1a 0; ammo 44223; acquire 44223 zm_aat_turned; "
                        "give_perk specialty_fastreload; switch 44223; ") == 0);
    if (strcmp(calls, "build kit 23; give 44223 1a 0; ammo 44223; acquire 44223 zm_aat_turned; "
                      "give_perk specialty_fastreload; switch 44223; ") != 0)
        printf("  calls: %s\n", calls);
    CHECK(strcmp(result, "Pack-a-Punch test: gave 1 of 2 guns, 1 ammo types, 1 perks") == 0);

    // the command itself: only "give bo3macfix_restore", from give's cheat check
    uint64_t give = base + ADDR_CheatsOk_From_Give, take = base + ADDR_CheatsOk_From_Take;
    CHECK(restore_command(give));
    CHECK(!restore_command(take));
    CHECK(!restore_command(base + 0xf4371d));
    argv1 = "ray_gun_upgraded";
    CHECK(!restore_command(give));
    argv1 = "bo3macfix_restore";
    munmap(image, Image_Size);
}

int main(void) {
    // a game image where ADDR_g_entities_Ptr is entities_address
    base = (uint64_t)&entities_address - ADDR_g_entities_Ptr;
    entities_address = (uint64_t)g_entities;
    orig_CheatsOk = game_cheats_ok;
    void *player = g_entities, *other = g_entities + Entity_Size;
    uint64_t give = base + ADDR_CheatsOk_From_Give, take = base + ADDR_CheatsOk_From_Take;
    uint64_t god = base + 0xf4371d; // another cheat command's call: god, at 0xf43718
    set_health(0, 100);
    set_health(1, 100);

    // nothing allowed: the game's answer
    CHECK(!let_through(player, give));

    // two allowed: give and take get through, nothing else does or uses them up
    zm_inventory_allow_cheats(2);
    CHECK(!let_through(player, god));
    CHECK(!let_through(other, give)); // another player in a game hosted here
    CHECK(!let_through(NULL, give));
    set_health(0, 0);
    CHECK(!let_through(player, give)); // dead, as the game's check wants too
    set_health(0, 100);
    CHECK(atomic_load(&allowed) == 2);
    CHECK(let_through(player, take));
    CHECK(let_through(player, give));
    CHECK(!let_through(player, give)); // both used
    CHECK(atomic_load(&allowed) == 0);

    // the hook itself, called from here rather than from give or take: the game's own check, allowance or not
    zm_inventory_allow_cheats(1);
    game_checks = 0;
    CHECK(fake_CheatsOk(player) == 0 && game_checks == 1);
    CHECK(atomic_load(&allowed) == 1);

    // only for a while
    atomic_store(&allowed_until, now_ms() - 1);
    CHECK(!let_through(player, give));

    // no g_entities to compare with: nothing gets through
    zm_inventory_allow_cheats(1);
    entities_address = 0;
    CHECK(!let_through(player, give));
    entities_address = (uint64_t)g_entities;
    CHECK(let_through(player, give));

    test_loadout();

    if (failures == 0)
        printf("test_zm_inventory: all passed\n");
    return failures == 0 ? 0 : 1;
}
