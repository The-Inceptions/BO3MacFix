// clientfield_guard.c: which callers of the game's client field lookup get a script error or a field that reads as 0
// when the field isn't there, which get the game's NULL, and the hook against a made-up lookup, script engine and mod
// (the test includes the source itself).
#include <setjmp.h>
#include <stdio.h>
#include <unistd.h>
#include "../source/clientfield_guard.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Base 0x100000000ULL

// --- the made-up game --------------------------------------------------------------------------------------------
static void *hooked_at, *hooked_with;
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)out_origin_func;
    hooked_at = address, hooked_with = fake_func, hooks++;
    return 0;
}

// the made-up mod: the folder the T7Overcharged stand-in was set up from, and the game's char[0x20] with the loaded
// mod's name. The guard asks neither, it acts with or without a mod; the tests set both ways to show that (and a
// mutant that asks again links against these)
static const char *t7o_folder = "";
const char *t7o_mod_folder(void);
const char *t7o_mod_folder(void) {
    return t7o_folder;
}
static char game_loaded_mod[0x20];

static const char *const game_set_names[Set_Count] = { "world", "actor", "vehicle", "allplayers", "allplayers",
    "toplayer", "playercorpse", "clientuimodel", "scriptmover", "helicopter", "plane", "missile", "zbarrier", "item" };

// the script strings: 0x10 + i is the i-th of these; 0x20 and 0x21 are two ids of one text, each with its own copy of
// it as the game's string table keeps them (DEIMOS's HUD made its name anew each time); 0x100 + i is "field_<i>"
static const char *const strings[] = { "hudItems.perks.additional_primary_weapon", "powerup_mini_gun.time",
                                       "t10_mule_kick" };
static char purifier_first[] = "powerup_purifier.time", purifier_again[] = "powerup_purifier.time";
#define Many_Fields 0x80
static char many_fields[Many_Fields][16];
static const char *game_string(uint32_t name) {
    if (name == 0x20)
        return purifier_first;
    if (name == 0x21)
        return purifier_again;
    if (name >= 0x100 && name < 0x100 + Many_Fields) {
        snprintf(many_fields[name - 0x100], sizeof(many_fields[0]), "field_%u", name - 0x100);
        return many_fields[name - 0x100];
    }
    return name >= 0x10 && name < 0x10 + sizeof(strings) / sizeof(strings[0]) ? strings[name - 0x10] : NULL;
}

// Scr_Error: throws to the script engine (here: back to the test), or comes back as with developer_script on
static jmp_buf script_engine;
static bool comes_back;
static int errors;
static uint32_t error_instance;
static bool error_terminal;
static char error_message[Message_Size];
static void game_script_error(uint32_t inst, const char *message, bool terminal) {
    errors++;
    error_instance = inst, error_terminal = terminal;
    snprintf(error_message, sizeof(error_message), "%s", message);
    if (!comes_back)
        longjmp(script_engine, 1);
}

// the lookup: one registered field, "powerup_mini_gun.time" in set 7, as the game's (set, name, error if missing)
static uint8_t registered_field[Field_Size];
static int lookups;
static uint64_t last_set, last_name, last_error;
static void *game_find(uint64_t set, uint64_t name, uint64_t error_if_missing) {
    lookups++;
    last_set = set, last_name = name, last_error = error_if_missing;
    return (int)set == 7 && (uint32_t)name == 0x11 ? registered_field : NULL;
}

// the game's int value getter and the float one's sum, as they read a field (offset +0x124, mask +0x128, shift
// +0x12c, bits +0x130)
static uint32_t read_int(const uint8_t *field, const uint8_t *values) {
    uint32_t offset, mask, word;
    memcpy(&offset, field + 0x124, 4), memcpy(&mask, field + 0x128, 4);
    memcpy(&word, values + offset, 4);
    return (word & mask) >> field[0x12c];
}
static float read_float(const uint8_t *field, const uint8_t *values) {
    int32_t bits;
    memcpy(&bits, field + 0x130, 4);
    return 1.0f / (float)((1 << bits) - 1) * (float)read_int(field, values);
}

// the thread's bgs, whose +8 points at the sets: set 7 holds the registered field
static uint8_t *game_sets;
static uint8_t game_bgs[16];
static uint8_t *game_bgs_pointer = game_bgs;
static uint8_t **game_thread_bgs(void) {
    return &game_bgs_pointer;
}

static void set_game(void) {
    if (game_sets == NULL) {
        game_sets = calloc(Set_Count, 0x4018);
        memcpy(game_bgs + 8, &game_sets, sizeof(game_sets));
        uint32_t one = 1, name = 0x11;
        uint8_t *field = registered_field;
        memcpy(game_sets + 7 * 0x4018 + 8, &one, 4);
        memcpy(game_sets + 7 * 0x4018 + 0x10, &field, sizeof(field));
        memcpy(registered_field + 0x120, &name, 4);
    }
    BG_ThreadBgs = game_thread_bgs;
    base_address = Base;
    set_names = game_set_names;
    SL_ConvertToString = game_string;
    Scr_Error = game_script_error;
    orig_ClientField_Find = game_find;
}

static void mod_loaded(bool loaded) {
    t7o_folder = loaded ? "/Users/x/Library/Application Support/Steam/steamapps/workshop/content/311210/2631943123"
                        : "";
    memset(game_loaded_mod, 0, sizeof(game_loaded_mod));
    if (loaded)
        memcpy(game_loaded_mod, "2631943123", 10);
}

// guarded_find as the hook calls it, from the place at offset; the result, or "thrown" when Scr_Error threw
static void *const Thrown = (void *)&script_engine;
static void *find_from(uint32_t offset, uint64_t set, uint64_t name, uint64_t error_if_missing) {
    if (setjmp(script_engine) != 0)
        return Thrown;
    void *found = game_find(set, name, error_if_missing);
    if (found != NULL)
        return found;
    return guarded_find(set, name, error_if_missing, Base + offset, found);
}

// what was printed while the block ran
static char printed[32768];
static int saved_stdout = -1;
static FILE *capture;
static void capture_start(void) {
    fflush(stdout);
    capture = tmpfile();
    saved_stdout = dup(STDOUT_FILENO);
    dup2(fileno(capture), STDOUT_FILENO);
}
static void capture_end(void) {
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    rewind(capture);
    size_t n = fread(printed, 1, sizeof(printed) - 1, capture);
    printed[n] = '\0';
    fclose(capture);
}
static int lines_in(const char *text) {
    int n = 0;
    for (; *text != '\0'; text++)
        n += *text == '\n';
    return n;
}

static void forget_seen(void) {
    memset(seen, 0, sizeof(seen));
    memset(seen_times, 0, sizeof(seen_times));
    seen_full = false;
}
static int count_in(const char *text, const char *what) {
    int n = 0;
    for (const char *p = text; (p = strstr(p, what)) != NULL; p++)
        n++;
    return n;
}

// --- the tests ---------------------------------------------------------------------------------------------------
static void test_sites(void) {
    // the 16 places, each by the instruction after its call, with its kind and script instance
    static const struct {
        uint32_t offset;
        int kind, instance;
        const char *t7o;
    } expected[] = {
        { 0x1293ef5, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault1" },
        { 0x1294286, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault2" },
        { 0x1294592, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault3" },
        { 0x12947e3, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault4" },
        { 0x129466a, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault5" },
        { 0x129435e, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault6" },
        { 0x1294877, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault7" },
        { 0x1294168, Clientfield_Script_Error, 0, "ScriptError_GscClientFieldFault8" },
        { 0x1294730, Clientfield_Script_Error, 0, "NonExistentClientfield_CrashRip" },
        { 0xef0719, Clientfield_Script_Error, 1, NULL },
        { 0xef07e9, Clientfield_Script_Error, 1, "ClientFieldFault_Csc1" },
        { 0xef0896, Clientfield_Script_Error, 1, "ClientFieldFault_Csc2" },
        { 0xef094f, Clientfield_Script_Error, 1, "ClientFieldFault_Csc3" },
        { 0x13b9d75, Clientfield_Zero, 0, "ClientFieldFault_MissingRip" },
        { 0x13b9e24, Clientfield_Zero, 0, "ClientFieldFault_NullRip1" },
        { 0x13b9e4c, Clientfield_Zero, 0, "ClientFieldFault_NullRip2" },
    };
    CHECK(Site_Count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const clientfield_site_t *site = clientfield_guard_site(expected[i].offset);
        CHECK(site != NULL);
        if (site == NULL)
            continue;
        CHECK(site->return_offset == expected[i].offset && site->kind == expected[i].kind &&
              site->script_instance == expected[i].instance && site->function != NULL);
        CHECK((site->t7o_name == NULL) == (expected[i].t7o == NULL));
        CHECK(site->t7o_name == NULL || strcmp(site->t7o_name, expected[i].t7o) == 0);
        // only the place itself, not the call before it or the instruction after
        CHECK(clientfield_guard_site(expected[i].offset - 5) == NULL &&
              clientfield_guard_site(expected[i].offset + 1) == NULL);
    }
    // the builtins that check for NULL themselves: SetClientField, IncrementClientField, SetUIModelClientField; the
    // registration's own lookup; any other caller
    CHECK(clientfield_guard_site(0x1293da6) == NULL && clientfield_guard_site(0x1294025) == NULL &&
          clientfield_guard_site(0x1294473) == NULL && clientfield_guard_site(0x13b986e) == NULL);
    CHECK(clientfield_guard_site(0) == NULL && clientfield_guard_site(UINT64_MAX) == NULL);
    CHECK(clientfield_guard_site((uint64_t)0x1294592 + ((uint64_t)1 << 32)) == NULL);
}

static void test_action(void) {
    const clientfield_site_t *error_site = clientfield_guard_site(ADDR_GScr_CodeGetUIModelClientField_Read);
    const clientfield_site_t *zero_site = clientfield_guard_site(ADDR_ClientField_GetInt_Read);
    CHECK(clientfield_guard_action(error_site, false, true) == Clientfield_Script_Error);
    CHECK(clientfield_guard_action(zero_site, false, true) == Clientfield_Zero);
    // a field that is there, a caller that may do without it, any other caller
    CHECK(clientfield_guard_action(error_site, true, true) == Clientfield_Pass);
    CHECK(clientfield_guard_action(error_site, false, false) == Clientfield_Pass);
    CHECK(clientfield_guard_action(NULL, false, true) == Clientfield_Pass);
    CHECK(clientfield_guard_action(zero_site, true, true) == Clientfield_Pass);
    CHECK(clientfield_guard_action(zero_site, false, false) == Clientfield_Pass);
}

static void test_set_name(void) {
    CHECK(strcmp(clientfield_guard_set_name(game_set_names, Set_Count, 0), "world") == 0);
    CHECK(strcmp(clientfield_guard_set_name(game_set_names, Set_Count, 7), "clientuimodel") == 0);
    CHECK(strcmp(clientfield_guard_set_name(game_set_names, Set_Count, 13), "item") == 0);
    CHECK(strcmp(clientfield_guard_set_name(game_set_names, Set_Count, 14), "unknown") == 0);
    CHECK(strcmp(clientfield_guard_set_name(game_set_names, Set_Count, -1), "unknown") == 0);
    CHECK(strcmp(clientfield_guard_set_name(NULL, Set_Count, 7), "unknown") == 0);
    const char *const holes[2] = { "world", NULL };
    CHECK(strcmp(clientfield_guard_set_name(holes, 2, 1), "unknown") == 0);
}

static void test_set_fields(void) {
    uint8_t *set = calloc(1, 0x4018);
    uint8_t first[Field_Size] = { 0 }, second[Field_Size] = { 0 }, third[Field_Size] = { 0 };
    uint32_t a = 0x11, b = 0x12, c = 0x99;
    memcpy(first + 0x120, &a, 4), memcpy(second + 0x120, &b, 4), memcpy(third + 0x120, &c, 4);
    uint8_t *fields[4] = { first, NULL, second, third };
    memcpy(set + 0x10, fields, sizeof(fields));
    char out[256];
    int32_t count = 4;
    memcpy(set + 8, &count, 4);
    clientfield_guard_set_fields(set, game_string, out, sizeof(out));
    CHECK(strcmp(out, "4 fields: powerup_mini_gun.time, ?, t10_mule_kick, ?") == 0);
    count = 0;
    memcpy(set + 8, &count, 4);
    clientfield_guard_set_fields(set, game_string, out, sizeof(out));
    CHECK(strcmp(out, "0 fields:") == 0);
    count = -3;
    memcpy(set + 8, &count, 4);
    clientfield_guard_set_fields(set, game_string, out, sizeof(out));
    CHECK(strcmp(out, "-3 fields:") == 0);
    // cut to the size given, and said so
    count = 4;
    memcpy(set + 8, &count, 4);
    clientfield_guard_set_fields(set, game_string, out, 20);
    CHECK(strlen(out) == 19 && strcmp(out + 16, "...") == 0 && strncmp(out, "4 fields: powe", 14) == 0);
    out[0] = 'x';
    clientfield_guard_set_fields(set, game_string, out, 0);
    CHECK(out[0] == 'x');
    // a count past what a set can hold: no more than its 0x800 places are read (the address sanitizer watches)
    count = 0x900;
    memcpy(set + 8, &count, 4);
    for (int i = 0; i < 0x800; i++)
        memcpy(set + 0x10 + 8 * i, &fields[0], sizeof(fields[0]));
    char *big = malloc(0x10000);
    clientfield_guard_set_fields(set, game_string, big, 0x10000);
    int listed = 0;
    for (const char *p = big; (p = strstr(p, "powerup_mini_gun.time")) != NULL; p++)
        listed++;
    CHECK(listed == 0x800 && strncmp(big, "2304 fields: ", 13) == 0);
    // and nothing after the last of them (the set's bit count follows its places)
    size_t length = strlen(big), last = strlen("powerup_mini_gun.time");
    CHECK(length > last && strcmp(big + length - last, "powerup_mini_gun.time") == 0);
    free(big);
    free(set);
}

static void test_key(void) {
    // the same place, set and text: the same key, whatever copy of the text it is
    CHECK(purifier_first != purifier_again);
    CHECK(clientfield_guard_key(2, 7, purifier_first) == clientfield_guard_key(2, 7, purifier_again));
    // another place, set or text: another key
    uint64_t key = clientfield_guard_key(2, 7, "powerup_purifier.time");
    CHECK(key != clientfield_guard_key(3, 7, "powerup_purifier.time"));
    CHECK(key != clientfield_guard_key(2, 6, "powerup_purifier.time"));
    CHECK(key != clientfield_guard_key(2, 7, "powerup_purifier.timf"));
    CHECK(key != clientfield_guard_key(2, 7, "powerup_purifier.tim"));
    CHECK(key != clientfield_guard_key(2, 7, ""));
    // every place with every set of the game, and the set past its table: all different
    uint64_t keys[16 * 15];
    int n = 0;
    for (size_t place = 0; place < 16; place++)
        for (int set = 0; set < 15; set++)
            keys[n++] = clientfield_guard_key(place, set, "t10_mule_kick");
    int same = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            same += keys[i] == keys[j];
    CHECK(same == 0);
    // FNV-1a's first steps, as written: the place, then the set, then each byte
    uint64_t expected = (0xcbf29ce484222325ULL ^ 1) * 0x100000001b3ULL;
    expected = (expected ^ 2) * 0x100000001b3ULL;
    expected = (expected ^ 'a') * 0x100000001b3ULL;
    CHECK(clientfield_guard_key(1, 2, "a") == expected);
    // a negative set is its 32 bits
    CHECK(clientfield_guard_key(1, -1, "a") == (((((0xcbf29ce484222325ULL ^ 1) * 0x100000001b3ULL) ^ 0xffffffffULL) *
                                                  0x100000001b3ULL) ^ 'a') * 0x100000001b3ULL);
}

static void test_first_time(void) {
    uint64_t keys[3] = { 0 };
    uint32_t counts[3] = { 7, 7, 7 }, times = 99;
    CHECK(clientfield_guard_first_time(keys, counts, 3, 5, &times) && times == 1);
    CHECK(!clientfield_guard_first_time(keys, counts, 3, 5, &times) && times == 2);
    CHECK(!clientfield_guard_first_time(keys, counts, 3, 5, &times) && times == 3);
    CHECK(clientfield_guard_first_time(keys, counts, 3, 6, &times) && times == 1);
    // key 0 is kept as 1, so it isn't taken for a free slot
    CHECK(clientfield_guard_first_time(keys, counts, 3, 0, &times) && times == 1);
    CHECK(!clientfield_guard_first_time(keys, counts, 3, 0, &times) && times == 2);
    // no room for a fourth: no line, and no count
    CHECK(!clientfield_guard_first_time(keys, counts, 3, 7, &times) && times == 0);
    CHECK(!clientfield_guard_first_time(keys, counts, 3, 5, &times) && times == 4);
}

static void test_guarded_find(void) {
    set_game();
    mod_loaded(true);
    forget_seen();
    uint8_t values[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    // a field that is there comes back from every place, untouched
    errors = 0;
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x11, 1) == registered_field && errors == 0);
    CHECK(find_from(ADDR_ClientField_GetInt_Read, 7, 0x11, 1) == registered_field);
    CHECK(guarded_find(7, 0x11, 1, Base + ADDR_GScr_CodeGetUIModelClientField_Read, registered_field) ==
          registered_field && errors == 0);

    // DEIMOS 935's: GetUIModelClientField of the server asks for one that isn't there
    capture_start();
    void *result = find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 1);
    capture_end();
    CHECK(result == Thrown && errors == 1 && error_instance == 0 && !error_terminal);
    CHECK(strcmp(error_message,
                 "No ClientField named hudItems.perks.additional_primary_weapon found in set clientuimodel") == 0);
    CHECK(lines_in(printed) == 1 && strstr(printed, "clientfield: GetUIModelClientField asked for "
                                                     "'hudItems.perks.additional_primary_weapon' in set "
                                                     "'clientuimodel'") != NULL &&
          strstr(printed, "a script error") != NULL);
    // again: the error again, the line only once
    capture_start();
    result = find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 1);
    capture_end();
    CHECK(result == Thrown && errors == 2 && printed[0] == '\0');
    // the same field from another place, another field from the same place: a line each
    capture_start();
    result = find_from(ADDR_GScr_CodeSetWorldClientField_Read, 7, 0x10, 1);
    void *second = find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x12, 1);
    capture_end();
    CHECK(result == Thrown && second == Thrown && errors == 4 && lines_in(printed) == 2);
    CHECK(strstr(printed, "SetWorldClientField asked for 'hudItems") != NULL && strstr(printed, "'t10_mule_kick'"));

    // one text under two script string ids, as DEIMOS's HUD asks for it: one line, and the count goes on
    capture_start();
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x20, 1);
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x21, 1);
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x20, 1);
    capture_end();
    CHECK(errors == 7 && lines_in(printed) == 1 && count_in(printed, "'powerup_purifier.time'") == 1);
    forget_seen();
    debug = true;
    capture_start();
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x20, 1);
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x21, 1);
    capture_end();
    debug = false;
    CHECK(count_in(printed, ", 1 times)") == 1 && count_in(printed, ", 2 times)") == 1 &&
          count_in(printed, "holds 1 fields:") == 1);
    errors = 4;

    // GetClientField's set depends on the entity: the same field of two sets from one place, a line each
    capture_start();
    find_from(ADDR_GScr_CodeGetClientField_Read, 1, 0x10, 1);
    find_from(ADDR_GScr_CodeGetClientField_Read, 2, 0x10, 1);
    capture_end();
    CHECK(errors == 6 && lines_in(printed) == 2 && strstr(printed, "in set 'actor'") != NULL &&
          strstr(printed, "in set 'vehicle'") != NULL);

    // a client's builtin: the client's script engine
    result = find_from(ADDR_CScr_CodeGetWorldClientField_Read, 0, 0x10, 1);
    CHECK(result == Thrown && errors == 7 && error_instance == 1 &&
          strcmp(error_message, "No ClientField named hudItems.perks.additional_primary_weapon found in set world") ==
              0);
    result = find_from(ADDR_CScr_CodeGetClientField_Read, 3, 0x12, 1);
    CHECK(result == Thrown && errors == 8 && error_instance == 1 &&
          strcmp(error_message, "No ClientField named t10_mule_kick found in set allplayers") == 0);
    // the set's name past the game's table, a script string without text
    result = find_from(ADDR_GScr_CodeGetWorldClientField_Read, 20, 0x99, 1);
    CHECK(result == Thrown && error_instance == 0 &&
          strcmp(error_message, "No ClientField named ? found in set unknown") == 0);

    // the value getters: the stand-in, which reads as 0 from any values, also as a float (not NaN)
    errors = 0;
    capture_start();
    uint8_t *field = find_from(ADDR_ClientField_GetInt_Read, 7, 0x10, 1);
    capture_end();
    CHECK(field == standin_field && errors == 0 && lines_in(printed) == 1 && strstr(printed, "reads as 0") != NULL &&
          strstr(printed, "the int value getter asked for 'hudItems.perks.additional_primary_weapon'") != NULL);
    CHECK(read_int(field, values) == 0 && read_float(field, values) == 0.0f);
    CHECK(find_from(ADDR_ClientField_GetFloat_Read, 7, 0x10, 1) == standin_field);
    CHECK(find_from(ADDR_ClientField_GetFloat_ReadBits, 7, 0x10, 1) == standin_field && errors == 0);
    uint32_t standin_bits;
    memcpy(&standin_bits, standin_field + 0x130, 4);
    CHECK(standin_bits == 1 && standin_field[0x12d] == 0 && sizeof(standin_field) == 0x140);
    for (size_t i = 0; i < sizeof(standin_field); i++)
        CHECK(i == 0x130 || standin_field[i] == 0);

    // a caller that may do without the field, even at a known place: the game's NULL
    errors = 0;
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 0) == NULL && errors == 0);
    CHECK(find_from(ADDR_ClientField_GetInt_Read, 7, 0x10, 0) == NULL);
    // only the byte the game tests counts for "error if missing"
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 0x100) == NULL && errors == 0);
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 0xff01) == Thrown && errors == 1);
    // the game's own checking builtins, any other caller: NULL
    errors = 0;
    CHECK(find_from(0x1294473, 7, 0x10, 1) == NULL && find_from(0x1293da6, 7, 0x10, 1) == NULL);
    CHECK(find_from(0x1294593, 7, 0x10, 1) == NULL && errors == 0);
    // the set is the low 32 bits of its register, as the game's lookup takes it
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 0xffffffff00000007ULL, 0x10, 1) == Thrown &&
          strcmp(error_message, "No ClientField named hudItems.perks.additional_primary_weapon found in set "
                                "clientuimodel") == 0);

    // without the mod (no T7Overcharged, no mod loaded): the same, a script error and a field that reads as 0
    mod_loaded(false);
    errors = 0;
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 1) == Thrown && errors == 1 &&
          strcmp(error_message, "No ClientField named hudItems.perks.additional_primary_weapon found in set "
                                "clientuimodel") == 0);
    CHECK(find_from(ADDR_ClientField_GetInt_Read, 7, 0x10, 1) == standin_field && errors == 1);
    CHECK(find_from(ADDR_CScr_CodeGetWorldClientField_Read, 0, 0x12, 1) == Thrown && errors == 2 &&
          error_instance == 1);
    // and still nothing for a field that is there, or for a caller that may do without one
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x11, 1) == registered_field);
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 0) == NULL && errors == 2);
    // another mod loaded, whose folder isn't T7Overcharged's: the same
    t7o_folder = "/a/b/2631943123";
    memcpy(game_loaded_mod, "1111111111", 10);
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 1) == Thrown && errors == 3);
    mod_loaded(true);
    errors = 0;

    // with developer_script on Scr_Error comes back: the caller gets the game's NULL
    comes_back = true;
    CHECK(find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x10, 1) == NULL && errors == 1);
    comes_back = false;

    // the debug lines: up to the 10th time, then each power of two
    forget_seen();
    debug = true;
    capture_start();
    for (int i = 0; i < 100; i++)
        find_from(ADDR_ClientField_GetInt_Read, 7, 0x12, 1);
    capture_end();
    debug = false;
    CHECK(strstr(printed, "clientfield: set 'clientuimodel' holds 1 fields: powerup_mini_gun.time\n") != NULL);
    CHECK(lines_in(printed) == 10 + 3 + 1 && strstr(printed, "ClientFieldFault_MissingRip") != NULL &&
          strstr(printed, ", 10 times)") != NULL && strstr(printed, ", 11 times)") == NULL &&
          strstr(printed, ", 16 times)") != NULL && strstr(printed, ", 64 times)") != NULL &&
          strstr(printed, ", 17 times)") == NULL);
    // a place without a T7O name
    forget_seen();
    debug = true;
    capture_start();
    find_from(ADDR_CScr_CodeGetClientField_Read, 7, 0x12, 1);
    capture_end();
    debug = false;
    CHECK(strstr(printed, "(T7Overcharged's -, game 0xef0719, 1 times)") != NULL);
}

// more different missing fields than the table holds: one line says so, once; the fields named before go on counting
static void test_full_table(void) {
    set_game();
    mod_loaded(true);
    forget_seen();
    errors = 0;
    capture_start();
    for (uint32_t i = 0; i < Seen_Size; i++)
        find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x100 + i, 1);
    capture_end();
    CHECK(errors == Seen_Size && lines_in(printed) == Seen_Size && count_in(printed, "different missing") == 0);
    CHECK(strstr(printed, "'field_0'") != NULL && strstr(printed, "'field_63'") != NULL);
    capture_start();
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x100 + Seen_Size, 1);
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x100 + Seen_Size + 1, 1);
    find_from(ADDR_ClientField_GetInt_Read, 7, 0x100 + Seen_Size + 2, 1);
    capture_end();
    CHECK(errors == Seen_Size + 2 && lines_in(printed) == 1 &&
          strcmp(printed, "clientfield: 64 different missing fields have been named here; any further one is guarded "
                          "the same way, without a line\n") == 0);
    // nor again later, and a field named before goes on being counted (its debug lines come)
    debug = true;
    capture_start();
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x100 + Seen_Size + 3, 1);
    find_from(ADDR_GScr_CodeGetUIModelClientField_Read, 7, 0x100, 1);
    capture_end();
    debug = false;
    CHECK(lines_in(printed) == 1 && strstr(printed, "asked for 'field_0'") != NULL &&
          strstr(printed, ", 2 times)") != NULL);
    forget_seen();
}

static void test_hook(void) {
    set_game();
    mod_loaded(true);
    errors = 0;
    lookups = 0;
    // a field that is there, with every argument handed on as it came
    CHECK(fake_ClientField_Find(0x1234567800000007ULL, 0xabcd00000011ULL, 0x5501) == registered_field);
    CHECK(lookups == 1 && last_set == 0x1234567800000007ULL && last_name == 0xabcd00000011ULL && last_error == 0x5501);
    // none, from here (not one of the game's places): the game's NULL
    CHECK(fake_ClientField_Find(7, 0x10, 1) == NULL && lookups == 2 && errors == 0);
}

static void test_init(void) {
    hooks = 0;
    clientfield_guard_init(Base, 0);
    CHECK(hooks == 0);
    base_address = 0;
    setenv("BO3MACFIX_CLIENTFIELDDEBUG", "1", 1);
    capture_start();
    clientfield_guard_init(Base, 1);
    capture_end();
    unsetenv("BO3MACFIX_CLIENTFIELDDEBUG");
    CHECK(hooks == 1 && hooked_at == (void *)(Base + ADDR_ClientField_Find) &&
          hooked_with == (void *)fake_ClientField_Find);
    CHECK(base_address == Base && debug &&
          set_names == (const char *const *)(Base + ADDR_ClientField_SetNames) &&
          (uint64_t)SL_ConvertToString == Base + ADDR_SL_ConvertToString &&
          (uint64_t)Scr_Error == Base + ADDR_Scr_Error && (uint64_t)BG_ThreadBgs == Base + ADDR_BG_ThreadBgs);
    CHECK(strstr(printed, "guarding 16 places") != NULL);
    capture_start();
    clientfield_guard_init(Base, 1);
    capture_end();
    CHECK(!debug && printed[0] == '\0');
    setenv("BO3MACFIX_CLIENTFIELDDEBUG", "0", 1);
    clientfield_guard_init(Base, 1);
    unsetenv("BO3MACFIX_CLIENTFIELDDEBUG");
    CHECK(!debug);
}

int main(void) {
    test_sites();
    test_action();
    test_set_name();
    test_key();
    test_first_time();
    test_set_fields();
    test_guarded_find();
    test_full_table();
    test_hook();
    test_init();
    if (failures != 0) {
        printf("test_clientfield_guard: %d failures\n", failures);
        return 1;
    }
    printf("test_clientfield_guard: all passed\n");
    return 0;
}
