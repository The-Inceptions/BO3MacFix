// Tests for zm_loadout.c: weapon names with attachments, the checkpoint.ini text for a loadout, reading the ammo types
// and perks from a made-up script VM's variables (laid out as the game's are), and the order a restore calls the game
// in, with made-up game calls.
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/zm_loadout.h"
#include "fake_vm.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- names

static void test_hash(void) {
    // the hashes the game's compiled scripts have for these (aat_shared.gsc, _zm_perks.gsc)
    CHECK(loadout_script_hash("aat") == 0x4db6bf4d);
    CHECK(loadout_script_hash("acquire") == 0x2ac07cf3);
    CHECK(loadout_script_hash("perks_active") == 0x9dc0d2b9);
    CHECK(loadout_script_hash("Perks_Active") == 0x9dc0d2b9); // any case
    CHECK(loadout_script_hash("weapon") == 0xdfcc01fd);
}

static void test_names(void) {
    CHECK(loadout_valid_name("ar_standard_upgraded_zm+reflex+extclip"));
    CHECK(loadout_valid_name("specialty_armorvest"));
    CHECK(!loadout_valid_name(""));
    CHECK(!loadout_valid_name("+reflex"));
    CHECK(!loadout_valid_name("ar_standard+"));
    CHECK(!loadout_valid_name("ar_standard++reflex"));
    CHECK(!loadout_valid_name("ray_gun;quit"));
    CHECK(!loadout_valid_name("ray gun"));
    CHECK(!loadout_valid_name("ray:gun"));

    static const char *const attachments[] = { "none", "acog", "damage", "dualclip", "dualoptic", "dw", "dynzoom",
                                               "extbarrel", "extclip", "fastads", "fastreload", "fmj" };
    int count = (int)(sizeof(attachments) / sizeof(attachments[0]));
    char out[Loadout_Name_Size];
    // index 0x123, then extclip (8) in the first field and fmj (11) in the third
    uint64_t weapon = 0x123 | (8ULL << 9) | (11ULL << 21);
    CHECK(loadout_weapon_name("ar_standard_upgraded_zm", weapon, attachments, count, out, sizeof(out)));
    CHECK(strcmp(out, "ar_standard_upgraded_zm+extclip+fmj") == 0);
    CHECK(loadout_weapon_name("ray_gun_upgraded_zm", 0x45, attachments, count, out, sizeof(out)));
    CHECK(strcmp(out, "ray_gun_upgraded_zm") == 0);
    // the last field (bits 51-56)
    CHECK(loadout_weapon_name("smg_fastfire_zm", 0x7 | (1ULL << 51), attachments, count, out, sizeof(out)));
    CHECK(strcmp(out, "smg_fastfire_zm+acog") == 0);
    // an id the table doesn't have, or a name that isn't one: nothing
    CHECK(!loadout_weapon_name("ar_standard_zm", 0x7 | (40ULL << 9), attachments, count, out, sizeof(out)));
    CHECK(out[0] == '\0');
    CHECK(!loadout_weapon_name("ar standard", 0x7, attachments, count, out, sizeof(out)));
    CHECK(!loadout_weapon_name("ar_standard+reflex", 0x7, attachments, count, out, sizeof(out)));
    char small[12];
    CHECK(!loadout_weapon_name("ar_standard_upgraded_zm", 0x7, attachments, count, small, sizeof(small)));

    // the other form of a name, attachments kept
    CHECK(loadout_other_name("ray_gun_upgraded_zm", out, sizeof(out)) && strcmp(out, "ray_gun_upgraded") == 0);
    CHECK(loadout_other_name("ray_gun_upgraded", out, sizeof(out)) && strcmp(out, "ray_gun_upgraded_zm") == 0);
    CHECK(loadout_other_name("ar_standard_upgraded_zm+reflex+extclip", out, sizeof(out)) &&
          strcmp(out, "ar_standard_upgraded+reflex+extclip") == 0);
    CHECK(loadout_other_name("ar_standard+reflex", out, sizeof(out)) && strcmp(out, "ar_standard_zm+reflex") == 0);
    CHECK(loadout_other_name("zm", out, sizeof(out)) && strcmp(out, "zm_zm") == 0);
    CHECK(!loadout_other_name("ray gun", out, sizeof(out)));
    CHECK(!loadout_other_name("ray_gun_upgraded", small, sizeof(small)));
}

// ---- checkpoint.ini

static void test_text(void) {
    loadout_t loadout;
    loadout_clear(&loadout);
    CHECK(loadout.held == -1 && loadout.gun_count == 0 && loadout.perk_count == 0);
    snprintf(loadout.guns[0].name, sizeof(loadout.guns[0].name), "ar_standard_upgraded_zm+reflex+extclip");
    loadout.guns[0].options = 0x822a;
    loadout.guns[0].acvi = 17;
    snprintf(loadout.guns[0].aat, sizeof(loadout.guns[0].aat), "zm_aat_dead_wire");
    snprintf(loadout.guns[1].name, sizeof(loadout.guns[1].name), "pistol_standard_zm");
    loadout.gun_count = 2;
    loadout.held = 0;
    snprintf(loadout.perks[0], sizeof(loadout.perks[0]), "specialty_armorvest");
    snprintf(loadout.perks[1], sizeof(loadout.perks[1]), "specialty_quickrevive");
    loadout.perk_count = 2;

    char guns[512], perks[256];
    CHECK(loadout_format_guns(&loadout, guns, sizeof(guns)));
    CHECK(strcmp(guns, "ar_standard_upgraded_zm+reflex+extclip:822a:17:zm_aat_dead_wire,pistol_standard_zm:0:0:") ==
          0);
    CHECK(loadout_format_perks(&loadout, perks, sizeof(perks)));
    CHECK(strcmp(perks, "specialty_armorvest,specialty_quickrevive") == 0);
    char small[16];
    CHECK(!loadout_format_guns(&loadout, small, sizeof(small)) && small[0] == '\0');

    loadout_t back;
    CHECK(loadout_parse(guns, "ar_standard_upgraded_zm+reflex+extclip", perks, &back));
    CHECK(back.gun_count == 2 && back.held == 0 && back.perk_count == 2);
    CHECK(strcmp(back.guns[0].name, loadout.guns[0].name) == 0 && back.guns[0].options == 0x822a &&
          back.guns[0].acvi == 17 && strcmp(back.guns[0].aat, "zm_aat_dead_wire") == 0);
    CHECK(strcmp(back.guns[1].name, "pistol_standard_zm") == 0 && back.guns[1].aat[0] == '\0');
    CHECK(strcmp(back.perks[1], "specialty_quickrevive") == 0);
    // the full 64 bits of options
    CHECK(loadout_parse("ray_gun_upgraded_zm:ffffffffffffffff:4294967295:", NULL, NULL, &back));
    CHECK(back.guns[0].options == UINT64_MAX && back.guns[0].acvi == UINT32_MAX && back.held == -1);
    // a gun in hand that isn't among them, or no lists at all
    CHECK(loadout_parse(guns, "ray_gun_zm", "", &back) && back.held == -1 && back.perk_count == 0);
    CHECK(loadout_parse(NULL, NULL, NULL, &back) && back.gun_count == 0);

    // malformed: nothing
    static const char *const bad_guns[] = {
        "ray_gun_zm:0:0",                  // a field short
        "ray_gun_zm:0:0:zm_aat_turned:x",  // one too many
        "ray_gun_zm:xyz:0:",               // options not hex
        ":0:0:",                           // no name
        "ray_gun_zm:0:-1:",                // acvi not a number
        "ray_gun_zm:0:4294967296:",        // too big for 32 bits
        "ray_gun_zm:10000000000000000:0:", // too big for 64 bits
        "ray_gun_zm:0:0:zm aat",           // an ammo type that isn't a name
        "ray_gun_zm;quit:0:0:",            // nothing but names goes into commands
        "ray_gun_zm:0:0:,",                // a trailing comma
        "a:0:0:,b:0:0:,c:0:0:,d:0:0:,e:0:0:,f:0:0:,g:0:0:,h:0:0:,i:0:0:", // more than Loadout_Max_Guns
    };
    for (size_t i = 0; i < sizeof(bad_guns) / sizeof(bad_guns[0]); i++) {
        CHECK(!loadout_parse(bad_guns[i], NULL, NULL, &back));
        CHECK(back.gun_count == 0);
    }
    CHECK(!loadout_parse(NULL, NULL, "specialty_armorvest,", &back));
    CHECK(!loadout_parse(NULL, NULL, "specialty armorvest", &back));
    CHECK(!loadout_parse(NULL, NULL, "specialty+armorvest", &back));
    CHECK(!loadout_parse(guns, "ray gun", perks, &back));
}

// ---- a made-up script VM, laid out as the game's (fake_vm.h), at a made-up address

#define Fake_Base 0x7f1200000000ULL
static uint8_t memory[FakeVm_Size];
static fake_vm_t vm;

static bool fake_read(uint64_t address, void *out, size_t size) {
    if (address < Fake_Base || address - Fake_Base + size > sizeof(memory))
        return false;
    memcpy(out, memory + (address - Fake_Base), size);
    return true;
}

static loadout_vm_t fake_loadout_vm(void) {
    return (loadout_vm_t){ fake_read, Fake_Base + FakeVm_Glob, Fake_Base + FakeVm_ClassMap,
                           Fake_Base + FakeVm_Strings };
}

static uint32_t player, aat_array, perks_array;
static const uint64_t raygun = 0x0a5, ar = 0x123 | (33ULL << 9) | (8ULL << 15), pistol = 0x04;

static void build_vm(void) {
    fake_vm_init(&vm, memory, Fake_Base);
    fake_vm_player(&vm, 1);
    player = fake_vm_player(&vm, 0);
    // self.aat: the ray gun's and the AR's, a removed one ("none"), one kept as a hash, one keyed by a number and one
    // that isn't a name
    fake_vm_child(&vm, player, loadout_script_hash("health_bar"), 4, 7, 7);
    aat_array = fake_vm_array_field(&vm, player, "aat");
    fake_vm_add_aat(&vm, aat_array, raygun, "zm_aat_blast_furnace");
    fake_vm_child(&vm, aat_array, fake_vm_entity(&vm, 5, ar), 5, loadout_script_hash("zm_aat_turned"), 5);
    fake_vm_add_aat(&vm, aat_array, pistol, "none");
    fake_vm_add_string(&vm, aat_array, 3, "zm_aat_fire_works");
    fake_vm_add_aat(&vm, aat_array, 0x77, "zm aat");
    // self.perks_active, with one twice and one that isn't a string
    perks_array = fake_vm_array_field(&vm, player, "perks_active");
    fake_vm_add_string(&vm, perks_array, 0, "specialty_armorvest");
    fake_vm_add_string(&vm, perks_array, 1, "specialty_quickrevive");
    fake_vm_add_string(&vm, perks_array, 2, "specialty_armorvest");
    fake_vm_child(&vm, perks_array, 3, 1, 12, 7);
}

static void test_vm(void) {
    build_vm();
    loadout_vm_t reader = fake_loadout_vm();
    CHECK(loadout_vm_entity(&reader, 0) == player);
    CHECK(loadout_vm_entity(&reader, 1) != 0 && loadout_vm_entity(&reader, 1) != player);
    CHECK(loadout_vm_entity(&reader, 2) == 0);
    CHECK(loadout_vm_entity(&reader, -1) == 0);
    CHECK(loadout_vm_array_field(&reader, player, "aat") == aat_array);
    CHECK(loadout_vm_array_field(&reader, player, "health_bar") == 0); // a number, not an array
    CHECK(loadout_vm_array_field(&reader, player, "nothing") == 0);

    loadout_aat_t aats[8];
    int count = loadout_vm_read_aats(&reader, player, aats, 8);
    CHECK(count == 2);
    CHECK(count >= 1 && aats[0].weapon == raygun && strcmp(aats[0].aat, "zm_aat_blast_furnace") == 0);
    CHECK(count >= 2 && aats[1].weapon == ar && strcmp(aats[1].aat, "zm_aat_turned") == 0);
    CHECK(loadout_vm_read_aats(&reader, player, aats, 1) == 1); // at most max

    char perks[8][Loadout_Short_Size];
    count = loadout_vm_read_strings(&reader, perks_array, perks, 8);
    CHECK(count == 2);
    CHECK(count == 2 && strcmp(perks[0], "specialty_armorvest") == 0 && strcmp(perks[1], "specialty_quickrevive") == 0);
    CHECK(loadout_vm_read_strings(&reader, aat_array + 1000, perks, 8) == -1); // not an array

    // per-client classes (client scripts) aren't for the server's players
    memory[FakeVm_Classes + 0x18] = 1;
    CHECK(loadout_vm_entity(&reader, 0) == 0);
    memory[FakeVm_Classes + 0x18] = 0;
    // a child whose parent isn't the array (freed and reused meanwhile): the walk stops there
    uint32_t first = fake_vm_get32(&vm, fake_vm_var(perks_array));
    fake_vm_put32(&vm, fake_vm_var(first) + 0x38, 9999);
    CHECK(loadout_vm_read_strings(&reader, perks_array, perks, 8) == -1);
    // memory that can't be read: nothing, no crash
    loadout_vm_t broken = reader;
    broken.var_glob = 0x10;
    CHECK(loadout_vm_entity(&broken, 0) == 0 && loadout_vm_read_aats(&broken, player, aats, 8) == -1);
    fake_vm_put64(&vm, FakeVm_Glob + 0x80, 0xdead0000);
    CHECK(loadout_vm_entity(&reader, 0) == 0);
}

// ---- restoring, against made-up game calls

typedef struct {
    uint64_t held[16];
    int held_count;
    char perks[8][Loadout_Short_Size];
    int perk_count;
    char calls[2048];
    bool fail_give, fail_aat;
} game_t;

static void call(game_t *game, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void call(game_t *game, const char *format, ...) {
    size_t used = strlen(game->calls);
    va_list args;
    va_start(args, format);
    vsnprintf(game->calls + used, sizeof(game->calls) - used, format, args);
    va_end(args);
    used = strlen(game->calls);
    snprintf(game->calls + used, sizeof(game->calls) - used, "; ");
}

static uint64_t g_weapon_for_name(void *context, const char *name) {
    (void)context;
    if (strcmp(name, "ray_gun_upgraded_zm") == 0)
        return raygun;
    if (strcmp(name, "ar_standard_upgraded_zm+reflex+extclip") == 0)
        return ar;
    if (strcmp(name, "pistol_standard_zm") == 0)
        return pistol;
    return 0;
}

static int g_primaries(void *context, uint64_t *out, int max) {
    game_t *game = context;
    int count = 0;
    for (int i = 0; i < game->held_count && count < max; i++)
        out[count++] = game->held[i];
    return count;
}

static bool g_take(void *context, uint64_t weapon) {
    game_t *game = context;
    call(game, "take %" PRIx64, weapon);
    return true;
}

static bool g_give(void *context, uint64_t weapon, uint64_t options, uint32_t acvi) {
    game_t *game = context;
    call(game, "give %" PRIx64 " %" PRIx64 " %u", weapon, options, acvi);
    return !game->fail_give;
}

static bool g_acquire_aat(void *context, uint64_t weapon, const char *aat) {
    game_t *game = context;
    call(game, "aat %" PRIx64 " %s", weapon, aat);
    return !game->fail_aat;
}

static bool g_has_perk(void *context, const char *perk) {
    game_t *game = context;
    for (int i = 0; i < game->perk_count; i++)
        if (strcmp(game->perks[i], perk) == 0)
            return true;
    return false;
}

static bool g_give_perk(void *context, const char *perk) {
    game_t *game = context;
    call(game, "perk %s", perk);
    return strcmp(perk, "specialty_unknown") != 0;
}

static void g_switch_to(void *context, uint64_t weapon) {
    game_t *game = context;
    call(game, "switch %" PRIx64, weapon);
}

static const char *restore(game_t *game, const char *guns, const char *in_hand, const char *perks,
                           loadout_result_t *result) {
    loadout_t saved;
    if (!loadout_parse(guns, in_hand, perks, &saved))
        return "(malformed)";
    loadout_ops_t ops = { game, g_weapon_for_name, g_primaries, g_take,      g_give,
                          g_acquire_aat, g_has_perk, g_give_perk, g_switch_to };
    game->calls[0] = '\0';
    loadout_restore(&saved, &ops, result);
    return game->calls;
}

static void test_restore(void) {
    game_t game;
    memset(&game, 0, sizeof(game));
    loadout_result_t result;
    // the starting pistol and an AAE gun held; the saved PaP'd guns given with their options, ammo types, perks
    game.held[0] = pistol;
    game.held[1] = 0x99;
    game.held_count = 2;
    snprintf(game.perks[0], sizeof(game.perks[0]), "specialty_quickrevive");
    game.perk_count = 1;
    const char *calls = restore(&game, "ar_standard_upgraded_zm+reflex+extclip:822a:17:zm_aat_dead_wire,"
                                       "ray_gun_upgraded_zm:2a:0:zm_aat_turned",
                                "ray_gun_upgraded_zm", "specialty_armorvest,specialty_quickrevive", &result);
    CHECK(strcmp(calls, "take 4; take 99; give 44323 822a 17; give a5 2a 0; aat 44323 zm_aat_dead_wire; "
                        "aat a5 zm_aat_turned; perk specialty_armorvest; switch a5; ") == 0);
    CHECK(result.taken == 2 && result.given == 2 && result.kept == 0 && result.aats == 2 && result.perks == 1 &&
          result.perks_had == 1 && result.failed == 0);
    char text[512];
    loadout_result_text(&result, text, sizeof(text));
    CHECK(strcmp(text, "took 2, gave 2 guns (0 held already), 2 ammo types, 1 perks (1 had already), 0 failed") == 0);

    // a saved gun that's held already stays, without being given again; no gun in hand saved: the first one
    calls = restore(&game, "pistol_standard_zm:0:0:,ray_gun_upgraded_zm:2a:0:", NULL, NULL, &result);
    CHECK(strcmp(calls, "take 99; give a5 2a 0; switch 4; ") == 0);
    CHECK(result.kept == 1 && result.given == 1 && result.taken == 1);

    // what the game doesn't have, or won't take, is told; the rest still comes back
    game.fail_aat = true;
    calls = restore(&game, "thundergun_zm:0:0:zm_aat_fire_works,ray_gun_upgraded_zm:2a:0:zm_aat_turned,"
                           "ray_gun_upgraded_zm:2a:0:", "thundergun_zm", "specialty_unknown", &result);
    CHECK(strcmp(calls, "take 4; take 99; give a5 2a 0; aat a5 zm_aat_turned; perk specialty_unknown; switch a5; ") ==
          0);
    CHECK(result.failed == 3 && result.given == 1 && result.aats == 0);
    CHECK(strcmp(result.failures, "no weapon thundergun_zm, no ammo type zm_aat_turned, no perk specialty_unknown") ==
          0);
    game.fail_aat = false;
    game.fail_give = true;
    calls = restore(&game, "ray_gun_upgraded_zm:2a:0:", NULL, NULL, &result);
    CHECK(result.failed == 1 && result.given == 0 && strstr(result.failures, "couldn't give ray_gun_upgraded_zm"));
    game.fail_give = false;

    // nothing saved: nothing taken either (an old checkpoint keeps the game's own guns)
    calls = restore(&game, "", NULL, "", &result);
    CHECK(strcmp(calls, "") == 0 && result.taken == 0);
}

int main(void) {
    test_hash();
    test_names();
    test_text();
    test_vm();
    test_restore();
    if (failures == 0)
        printf("test_zm_loadout: all passed\n");
    return failures == 0 ? 0 : 1;
}
