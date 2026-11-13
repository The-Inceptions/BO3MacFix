// t7o_guards.c: the weapon attachment guard against a made-up version of the game's field lookup, the local client
// guard against a made-up controller lookup, the log's rule and what init installs (the test includes the source
// itself). The detour's thunk is x86_64 code with a test of its own (test_detour.c); here its handler is called the
// way the thunk calls it, with the registers the game has at the place.
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/t7o_guards.c"

// ---- what the code asks of the rest of BO3MacFix

static char mod_folder[64]; // "" until the mod has loaded T7Overcharged
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static bool have_stubs;
bool prepatch_has_hook(uint32_t offset) {
    (void)offset;
    return have_stubs;
}
static struct {
    void *address, *handler;
} hooked[4];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)out_origin_func;
    if (hooks < 4)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func };
    hooks++;
    return 0;
}

// ---- the made-up game: a weapon's definition with its table of attachment entries (at +0x40), and the function that
// works out which attachment gives a field's value (the Mac's 0x101381200), reduced to what the guard is about

#define Fields 197
// where the game reads an attachment entry's override bits ("mov ecx, [rax + 4*rcx + 0x3c0]"), its own number here
#define Game_Overrides 0x3c0
typedef struct {
    uint8_t bytes[0x400];
} entry_t;
static entry_t entries[64];
static entry_t *table[64]; // the definition's table: NULL where the weapon has no entry for an attachment

static void overrides(int attachment, int field) {
    uint32_t bits;
    memcpy(&bits, entries[attachment].bytes + Game_Overrides + 4 * (field >> 5), sizeof(bits));
    bits |= 1u << (field & 31);
    memcpy(entries[attachment].bytes + Game_Overrides + 4 * (field >> 5), &bits, sizeof(bits));
}
// the game's read, byte by byte through a volatile pointer: the compiler can't fold a read of the stand-in (all
// zeros) into a constant, so a read past its end is one the address sanitizer sees
static bool overridden(const uint8_t *entry, int field) {
    const volatile uint8_t *word = entry + Game_Overrides + 4 * (field >> 5);
    uint32_t bits = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
    return (bits >> (field & 31) & 1) != 0;
}

// a weapon: its number in the low 9 bits, then its attachments, 6 bits each
static uint64_t weapon(unsigned number, int first, int second) {
    return number | (uint64_t)first << 9 | (uint64_t)second << 15;
}

#define Crashed (-1)
static int handler_calls;
// the attachment whose value the game takes for field: mode is the weapon's mode attachment (CG_RegisterWeapon's
// fourth argument). Crashed where the game would read through a missing entry.
static int field_from(uint64_t packed, int field, int mode) {
    if (((unsigned)(field - 0x68) < 5 || (unsigned)(field - 0xad) < 3) && mode != 0) {
        // the game's index helper gives the attachment itself when its entry is missing
        detour_registers_t registers;
        memset(&registers, 0x5a, sizeof(registers));
        registers.rax = (uint64_t)table[mode];
        registers.rcx = (uint64_t)(field >> 5);
        registers.r12 = (uint64_t)field;
        registers.rbx = (uint64_t)mode;
        registers.r15 = packed;
        detour_registers_t before = registers;
        handler_calls++;
        handler_BG_WeaponAttachment_Overrides(&registers);
        // nothing but the entry the game reads changes
        before.rax = registers.rax;
        CHECK(memcmp(&before, &registers, sizeof(registers)) == 0);
        const uint8_t *entry = (const uint8_t *)registers.rax;
        if (entry == NULL)
            return Crashed;
        if (overridden(entry, field))
            return mode;
    }
    // the weapon's own attachments, each looked up with a null test, as the game does
    for (int shift = 9; shift <= 15; shift += 6) {
        int attachment = (int)(packed >> shift & 63);
        if (attachment != 0 && table[attachment] != NULL && overridden(table[attachment]->bytes, field))
            return attachment;
    }
    return 0;
}

static void reset_weapon(void) {
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < 64; i++)
        table[i] = &entries[i];
}

static void test_attachment_entry(void) {
    entry_t there;
    // an entry that is there is read as it is, with the mod or without, whatever the word
    for (uint64_t word = 0; word < 9; word++) {
        CHECK(t7o_guards_attachment_entry(&there, word, true) == &there);
        CHECK(t7o_guards_attachment_entry(&there, word, false) == &there);
    }
    // a missing one: the stand-in, whose bits are all clear for every field, once the mod is loaded
    for (int field = 0; field < Fields; field++) {
        uint64_t word = (uint64_t)(field >> 5);
        const uint8_t *stand_in = t7o_guards_attachment_entry(NULL, word, true);
        CHECK(stand_in != NULL);
        if (stand_in != NULL)
            CHECK(!overridden(stand_in, field));
        CHECK(t7o_guards_attachment_entry(NULL, word, false) == NULL);
    }
    // a word past the 197 fields' would be read from behind the stand-in: the game's own read then
    CHECK(t7o_guards_attachment_entry(NULL, Override_Words, true) == NULL);
    CHECK(t7o_guards_attachment_entry(NULL, UINT64_MAX, true) == NULL);
}

static void test_weapon_fields(void) {
    strcpy(mod_folder, "/mods/aae");
    reset_weapon();
    uint64_t gun = weapon(0x123, 3, 9);
    // the mode attachment (7) overrides field 0x69: its value
    overrides(7, 0x69);
    overrides(3, 0x69);
    CHECK(field_from(gun, 0x69, 7) == 7);
    // it doesn't override 0x6a, the weapon's attachment 9 does
    overrides(9, 0x6a);
    CHECK(field_from(gun, 0x6a, 7) == 9);
    CHECK(attachments_missing == 0);

    // the mode attachment has no entry: the game would crash; with the guard it's one that overrides nothing,
    // so the field comes from where it comes for an entry without the field's bit
    for (int field = 0; field < Fields; field++) {
        reset_weapon();
        overrides(3, field);
        entries[7] = (entry_t){ 0 };
        int without_bit = field_from(gun, field, 7);
        table[7] = NULL;
        CHECK(field_from(gun, field, 7) == without_bit);
        reset_weapon();
        entries[7] = (entry_t){ 0 };
        without_bit = field_from(gun, field, 7);
        table[7] = NULL;
        CHECK(field_from(gun, field, 7) == without_bit);
    }
    // only the eight fields that ask the mode attachment come through the place: 2 lookups each, 2 with it missing
    CHECK(attachments_missing == 2 * 8);
    CHECK(handler_calls == 4 * 8 + 2);

    // without the mod the game's own read runs, and crashes
    reset_weapon();
    table[7] = NULL;
    mod_folder[0] = '\0';
    CHECK(field_from(gun, 0x6b, 7) == Crashed);
    CHECK(field_from(gun, 0xae, 7) == Crashed);
    CHECK(attachments_missing == 2 * 8);
    strcpy(mod_folder, "/mods/aae");
    CHECK(field_from(gun, 0xae, 7) == 0);
    // a word of bits past the stand-in's stays the game's
    detour_registers_t registers = { 0 };
    registers.rcx = Override_Words;
    handler_BG_WeaponAttachment_Overrides(&registers);
    CHECK(registers.rax == 0);
    CHECK(attachments_missing == 2 * 8 + 1);
}

// the made-up controller lookup: the game's table of 2, and what it reads outside it
static int game_lookups;
static int32_t game_controller_index(int32_t local_client) {
    game_lookups++;
    static const int32_t controllers[2] = { 0, 3 };
    return local_client == 0 || local_client == 1 ? controllers[local_client] : 0x5a5a;
}

static void test_local_clients(void) {
    CHECK(t7o_guards_local_client_in_table(0) && t7o_guards_local_client_in_table(1));
    CHECK(!t7o_guards_local_client_in_table(2) && !t7o_guards_local_client_in_table(-1));
    CHECK(!t7o_guards_local_client_in_table(INT_MIN) && !t7o_guards_local_client_in_table(INT_MAX));

    orig_Com_LocalClient_GetControllerIndex = game_controller_index;
    strcpy(mod_folder, "/mods/aae");
    // the two local clients: the game's own answer
    CHECK(fake_Com_LocalClient_GetControllerIndex(0) == 0 && fake_Com_LocalClient_GetControllerIndex(1) == 3);
    CHECK(game_lookups == 2);
    // anything else: no controller, without the game reading outside its table
    CHECK(fake_Com_LocalClient_GetControllerIndex(-1) == -1);
    CHECK(fake_Com_LocalClient_GetControllerIndex(-1) == -1);
    CHECK(fake_Com_LocalClient_GetControllerIndex(2) == -1);
    CHECK(fake_Com_LocalClient_GetControllerIndex(-2) == -1);
    CHECK(fake_Com_LocalClient_GetControllerIndex(INT_MAX) == -1);
    CHECK(fake_Com_LocalClient_GetControllerIndex(INT_MIN) == -1);
    CHECK(game_lookups == 2);
    CHECK(no_local_client == 2 && local_clients_out_of_table == 4);
    // without the mod the game's own lookup runs for every number
    mod_folder[0] = '\0';
    CHECK(fake_Com_LocalClient_GetControllerIndex(-1) == 0x5a5a);
    CHECK(fake_Com_LocalClient_GetControllerIndex(2) == 0x5a5a);
    CHECK(fake_Com_LocalClient_GetControllerIndex(1) == 3);
    CHECK(game_lookups == 5);
    CHECK(no_local_client == 2 && local_clients_out_of_table == 4);
}

static void test_log(void) {
    int lines = 0;
    for (uint32_t n = 1; n <= 100000; n++)
        lines += t7o_guards_logged(n);
    CHECK(lines == 16 + 12 && t7o_guards_logged(16) && !t7o_guards_logged(17) && t7o_guards_logged(32) &&
          !t7o_guards_logged(48) && t7o_guards_logged(65536));
}

static void test_init(void) {
    const uint64_t base = 0x100000000ULL;
    // off: nothing
    hooks = 0;
    have_stubs = true;
    t7o_guards_init(base, 0);
    CHECK(hooks == 0);
    // on, from the pre-patched executable: the hook on the lookup and the detour at the read
    t7o_guards_init(base, 1);
    CHECK(hooks == 2);
    CHECK(hooked[0].address == (void *)(base + ADDR_Com_LocalClient_GetControllerIndex) &&
          hooked[0].handler == (void *)fake_Com_LocalClient_GetControllerIndex);
    CHECK(hooked[1].address == (void *)(base + ADDR_BG_WeaponAttachment_Overrides) &&
          hooked[1].handler == (void *)handler_BG_WeaponAttachment_Overrides);
    // not from it: only the hook, a detour inside a function needs the copy's stub
    hooks = 0;
    have_stubs = false;
    t7o_guards_init(base, 1);
    CHECK(hooks == 1 && hooked[0].address == (void *)(base + ADDR_Com_LocalClient_GetControllerIndex));
}

int main(void) {
    test_attachment_entry();
    test_weapon_fields();
    test_local_clients();
    test_log();
    test_init();
    if (failures != 0) {
        printf("test_t7o_guards: %d failures\n", failures);
        return 1;
    }
    printf("test_t7o_guards: all passed\n");
    return 0;
}
