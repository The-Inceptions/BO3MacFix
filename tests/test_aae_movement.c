// aae_movement.c: T7Overcharged's movement switches (its commands' rules, its start, its dvars, what the movement code
// is told) and its fast ADS against a made-up game.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define ARGC(argv) ((int)(sizeof(argv) / sizeof((argv)[0])))

#include "../source/aae_movement.c"

// the sums below are compared with T7Overcharged's to the bit: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

// ---- what the code asks of the rest of BO3MacFix

static bool mod_loaded = true; // the loaded mod is All-around Enhancement with its plugin
static uint64_t plugin_base;
void aae_plugin_init(uint64_t game_base_address) {
    plugin_base = game_base_address;
}
bool aae_plugin_loaded_lately(void) {
    return mod_loaded;
}
// which places the pre-patched executable has stubs for, and what was hooked
static bool have_stubs = true;
static int stub_missing_for = -1;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && (int)offset != stub_missing_for;
}
static struct {
    void *address, *replacement, **original;
} hooked[8];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    return 0;
}
// what a detour's place is pointed at: its thunk where the build has them (x86_64), else the handler itself
#if defined(__x86_64__)
#define DETOUR_ENTRY(name) ((void *)thunk_##name)
#else
#define DETOUR_ENTRY(name) ((void *)handler_##name)
#endif
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

// ---- the made-up game: its console commands

static struct {
    const char *name;
    void (*function)(void);
    void *command;
} game_commands[4];
static int game_command_count;
static const char *game_has_command; // one the game has already, or NULL
static uint8_t game_own_command[0x30];

static void *game_find_command(const char *name) {
    for (int i = 0; i < game_command_count; i++)
        if (strcmp(game_commands[i].name, name) == 0)
            return game_commands[i].command;
    return game_has_command != NULL && strcmp(game_has_command, name) == 0 ? game_own_command : NULL;
}
static void game_add_command(const char *name, void (*function)(void), void *command) {
    if (game_command_count < 4)
        game_commands[game_command_count] = (typeof(game_commands[0])){ name, function, command };
    game_command_count++;
}
static void (*command_named(const char *name))(void) {
    for (int i = 0; i < game_command_count && i < 4; i++)
        if (strcmp(game_commands[i].name, name) == 0)
            return game_commands[i].function;
    return NULL;
}

// the arguments of the command being run, where the game keeps them: the thread's block, whose +0x18 points at
// them (a nesting level, then an argc and an argv for each level)
static _Alignas(8) uint8_t game_tls[0x40];
static _Alignas(8) uint8_t game_args[0x100];
static bool no_tls;
static uint8_t *game_get_tls(void) {
    return no_tls ? NULL : game_tls;
}
static void run_at(int nesting, int argc, const char *const *argv, const char *command) {
    void *args = game_args;
    memcpy(game_tls + 0x18, &args, sizeof(args));
    memset(game_args, 0, sizeof(game_args));
    memcpy(game_args, &nesting, sizeof(nesting));
    // also for a level the game doesn't have (-1, 8), so that reading it would show
    if (nesting >= -1 && nesting <= 8) {
        memcpy(game_args + 0x64 + 4 * nesting, &argc, sizeof(argc));
        memcpy(game_args + 0x88 + 8 * nesting, &argv, sizeof(argv));
    }
    void (*function)(void) = command_named(command);
    CHECK(function != NULL);
    if (function != NULL)
        function();
}
// the console runs one of the two commands, with these words
static void run(int argc, const char *const *argv) {
    run_at(0, argc, argv, argv[0]);
}
#define RUN(...)                                                                                                       \
    do {                                                                                                               \
        const char *words_[] = { __VA_ARGS__ };                                                                        \
        run(ARGC(words_), words_);                                                                                     \
    } while (0)

// ---- its dvars: dvar_t as the code reads it (the name at +8, the current value at +0x28), and what the game keeps

typedef struct {
    _Alignas(8) uint8_t game_side[0x80];
    uint32_t hash;
    int type; // 1 bool, 2 float, 6 int, 8 text the mod set before anybody registered it ("External Dvar")
    char text[32];
    float min, max, value_default;
    int int_min, int_max, int_default;
    bool bool_default;
    uint32_t flags;
    const char *description;
    int registrations, order; // how often it was registered, and as which one of all
} game_dvar_t;
static game_dvar_t game_dvars[8];
static int game_dvar_count, game_registrations;
static bool game_refuses_registration; // the game has no room for another dvar
static bool game_refuses_floats;       // the same from the first float on

static game_dvar_t *game_dvar(const char *name) {
    uint32_t hash = gsc_hash(name);
    for (int i = 0; i < game_dvar_count; i++)
        if (game_dvars[i].hash == hash)
            return &game_dvars[i];
    return NULL;
}
static game_dvar_t *game_dvar_made(uint32_t hash) {
    for (int i = 0; i < game_dvar_count; i++)
        if (game_dvars[i].hash == hash)
            return &game_dvars[i];
    game_dvar_t *dvar = &game_dvars[game_dvar_count++];
    memset(dvar, 0, sizeof(*dvar));
    dvar->hash = hash;
    return dvar;
}
// the mod's SetDvar on a dvar nobody registered: the game keeps the text
static void set_before(const char *name, const char *text) {
    game_dvar_t *dvar = game_dvar_made(gsc_hash(name));
    dvar->type = 8;
    snprintf(dvar->text, sizeof(dvar->text), "%s", text);
}
static void registered(game_dvar_t *dvar, int type, uint32_t flags, const char *description) {
    dvar->type = type;
    dvar->flags = flags;
    dvar->description = description;
    dvar->registrations++;
    dvar->order = ++game_registrations;
}
// The game's registrations: a dvar that was set before as text gets the text as a value, put inside the domain (the
// game's 0x100fa4dc5 with 0x100fa30ff and 0x100fa00b5: atof or atoi, below the least the least, above the most the
// most); one registered before keeps its value; a new one gets the default.
static uint8_t *game_register_float(uint32_t hash, const char *name, float value, float min, float max,
                                    uint32_t flags, const char *description) {
    (void)name;
    if (game_refuses_registration || game_refuses_floats)
        return NULL;
    game_dvar_t *dvar = game_dvar_made(hash);
    float current = value;
    if (dvar->type == 8) {
        current = (float)atof(dvar->text);
        if (min > current)
            current = min;
        else if (current > max)
            current = max;
    } else if (dvar->type == 2) {
        memcpy(&current, dvar->game_side + 0x28, sizeof(current));
    }
    memcpy(dvar->game_side + 0x28, &current, sizeof(current));
    dvar->min = min, dvar->max = max, dvar->value_default = value;
    registered(dvar, 2, flags, description);
    return dvar->game_side;
}
static uint8_t *game_register_int(uint32_t hash, const char *name, int value, int min, int max, uint32_t flags,
                                  const char *description) {
    (void)name;
    if (game_refuses_registration)
        return NULL;
    game_dvar_t *dvar = game_dvar_made(hash);
    int current = value;
    if (dvar->type == 8) {
        current = atoi(dvar->text);
        if (current < min)
            current = min;
        else if (current > max)
            current = max;
    } else if (dvar->type == 6) {
        memcpy(&current, dvar->game_side + 0x28, sizeof(current));
    }
    memcpy(dvar->game_side + 0x28, &current, sizeof(current));
    dvar->int_min = min, dvar->int_max = max, dvar->int_default = value;
    registered(dvar, 6, flags, description);
    return dvar->game_side;
}
static uint8_t *game_register_bool(uint32_t hash, const char *name, bool value, uint32_t flags,
                                   const char *description) {
    (void)name;
    if (game_refuses_registration)
        return NULL;
    game_dvar_t *dvar = game_dvar_made(hash);
    bool current = value;
    if (dvar->type == 8)
        current = atoi(dvar->text) != 0;
    else if (dvar->type == 1)
        current = dvar->game_side[0x28] != 0;
    dvar->game_side[0x28] = current;
    dvar->bool_default = value;
    registered(dvar, 1, flags, description);
    return dvar->game_side;
}
// the mod's SetDvar on a registered dvar: the game refuses a value outside the domain and keeps the one it had
static bool set_float(const char *name, float value) {
    game_dvar_t *dvar = game_dvar(name);
    if (dvar == NULL || dvar->type != 2 || dvar->min > value || value > dvar->max)
        return false;
    memcpy(dvar->game_side + 0x28, &value, sizeof(value));
    return true;
}
static void set_bool(const char *name, bool value) {
    game_dvar_t *dvar = game_dvar(name);
    if (dvar != NULL)
        dvar->game_side[0x28] = value;
}
// the mod's SetDvar, whichever of the two it is
static void mod_sets(const char *name, const char *text) {
    game_dvar_t *dvar = game_dvar(name);
    if (dvar == NULL || dvar->type == 8) {
        set_before(name, text);
    } else if (dvar->type == 2) {
        set_float(name, (float)atof(text));
    } else if (dvar->type == 6) {
        int value = atoi(text);
        if (value >= dvar->int_min && value <= dvar->int_max)
            memcpy(dvar->game_side + 0x28, &value, sizeof(value));
    } else if (dvar->type == 1) {
        set_bool(name, atoi(text) != 0);
    }
}
// whatever the dvar holds (what no SetDvar could put there, for the rules)
static void put_float(const char *name, float value) {
    game_dvar_t *dvar = game_dvar(name);
    if (dvar != NULL)
        memcpy(dvar->game_side + 0x28, &value, sizeof(value));
}
static float float_of(const char *name) {
    game_dvar_t *dvar = game_dvar(name);
    float value = -1.0f;
    if (dvar != NULL)
        memcpy(&value, dvar->game_side + 0x28, sizeof(value));
    return value;
}
static int int_of(const char *name) {
    game_dvar_t *dvar = game_dvar(name);
    int value = -1;
    if (dvar != NULL)
        memcpy(&value, dvar->game_side + 0x28, sizeof(value));
    return value;
}
// the game's way of naming a dvar: its own copy of the text at +8, the one there released first
static char game_strings[16][40];
static int game_string_count, game_names_released, game_namings;
static void game_set_name(uint8_t *dvar, const char *name) {
    const char *old;
    memcpy(&old, dvar + 8, sizeof(old));
    if (old != NULL)
        game_names_released++;
    char *copy = game_strings[game_string_count++ % 16];
    snprintf(copy, sizeof(game_strings[0]), "%s", name);
    memcpy(dvar + 8, &copy, sizeof(copy));
    game_namings++;
}
// whether a pointer is one of the game's own strings
static bool game_string(const char *text) {
    return text >= &game_strings[0][0] && text < &game_strings[0][0] + sizeof(game_strings);
}
static const char *name_in(const char *name) {
    game_dvar_t *dvar = game_dvar(name);
    const char *in = NULL;
    if (dvar != NULL)
        memcpy(&in, dvar->game_side + 8, sizeof(in));
    return in;
}

static uint64_t made_up_base;
static const int game_sniper_anim_types[2] = { 7, 8 };
static void made_up_game(uint64_t base) {
    made_up_base = base;
    sniper_anim_types = game_sniper_anim_types;
    Cmd_FindCommand = game_find_command;
    Cmd_AddCommandInternal = game_add_command;
    Sys_GetTLS = game_get_tls;
    Dvar_RegisterInt = game_register_int;
    Dvar_RegisterFloat = game_register_float;
    Dvar_RegisterBool = game_register_bool;
    Dvar_SetName = game_set_name;
}

// ---- what the switches did, in order

static struct {
    move_patch patch;
    bool on, changed;
    float multiplier, before;
} seen[64];
static int seen_count;
static void record_switch(move_patch patch, bool on, bool changed, float multiplier, float before) {
    if (seen_count < 64)
        seen[seen_count] = (typeof(seen[0])){ patch, on, changed, multiplier, before };
    seen_count++;
}

// ---- what is printed

static char printed[65536];
static int saved_stdout = -1;
static FILE *print_file;
static void printing_begin(void) {
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    print_file = tmpfile();
    dup2(fileno(print_file), STDOUT_FILENO);
}
static const char *printing_end(void) {
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    rewind(print_file);
    size_t length = fread(printed, 1, sizeof(printed) - 1, print_file);
    printed[length] = '\0';
    fclose(print_file);
    return printed;
}

// everything as it is before the game starts, the setting on
static void reset(void) {
    atomic_store(&started, false);
    atomic_store(&start_looked_at, false);
    atomic_store(&switches.state, (uint64_t)float_bits(2.15f) << 32);
    atomic_store(&omni_dvar, NULL);
    atomic_store(&strafe_dvar, NULL);
    atomic_store(&fastads_dvar, NULL);
    memset(game_dvars, 0, sizeof(game_dvars));
    game_dvar_count = game_registrations = 0;
    game_string_count = game_names_released = game_namings = 0;
    game_refuses_registration = game_refuses_floats = false;
    game_command_count = 0;
    game_has_command = NULL;
    no_tls = false;
    seen_count = 0;
    mod_loaded = true;
    enabled = true;
    forced = debug = false;
    switched = record_switch;
    find_game = made_up_game;
    hooks = 0;
    have_stubs = true;
    stub_missing_for = -1;
    atomic_store(&fastads_lines, 0);
    atomic_store(&fastads_rate_said, UINT64_MAX);
    memset(fastads_reached, 0, sizeof(fastads_reached));
    memset(&perk_test_said, 0, sizeof(perk_test_said));
    perk_test_said.player_state = UINT64_MAX;
    unsetenv("BO3MACFIX_MOVEFASTADSPERK");
    unsetenv("BO3MACFIX_MOVEFORCE");
    unsetenv("BO3MACFIX_MOVEDEBUG");
    aae_movement_init(0x100000000, 1);
    seen_count = 0;
    fastads_perk_forced = false;
}

// the patches' bits as the commands left them
static uint32_t switch_bits(void) {
    return (uint32_t)atomic_load(&switches.state);
}
static bool only_on(uint32_t patches) {
    for (int patch = 0; patch < Move_Patch_Count; patch++)
        if (aae_movement_patch_on((move_patch)patch) != ((patches >> patch & 1) != 0))
            return false;
    return true;
}
#define ALL_PATCHES ((1u << Move_Patch_Count) - 1)
static bool same_float(float a, float b) {
    return float_bits(a) == float_bits(b);
}

// ---- the rules

static void test_stoi(void) {
    static const struct {
        const char *text;
        move_number result;
        int value;
    } cases[] = {
        { "1", Move_Number_Ok, 1 },
        { "0", Move_Number_Ok, 0 },
        { "-1", Move_Number_Ok, -1 },
        { "+7", Move_Number_Ok, 7 },
        { " 1", Move_Number_Ok, 1 },
        { "\t\n\v\f\r 5", Move_Number_Ok, 5 },
        { "1abc", Move_Number_Ok, 1 },
        { "12 34", Move_Number_Ok, 12 },
        { "0x10", Move_Number_Ok, 0 },
        { "007", Move_Number_Ok, 7 },
        { "-0", Move_Number_Ok, 0 },
        { "1.9", Move_Number_Ok, 1 },
        { "000000000000000000000000001", Move_Number_Ok, 1 },
        { "2147483647", Move_Number_Ok, INT32_MAX },
        { "-2147483648", Move_Number_Ok, INT32_MIN },
        { "+2147483647", Move_Number_Ok, INT32_MAX },
        { "2147483648", Move_Number_Out_Of_Range, 0 },
        { "-2147483649", Move_Number_Out_Of_Range, 0 },
        { "4294967296", Move_Number_Out_Of_Range, 0 },
        { "4294967297", Move_Number_Out_Of_Range, 0 },
        { "21474836470", Move_Number_Out_Of_Range, 0 },
        { "99999999999999999999999999999", Move_Number_Out_Of_Range, 0 },
        { "-99999999999999999999999999999", Move_Number_Out_Of_Range, 0 },
        { "", Move_Number_None, 0 },
        { " ", Move_Number_None, 0 },
        { "abc", Move_Number_None, 0 },
        { "on", Move_Number_None, 0 },
        { "+", Move_Number_None, 0 },
        { "-", Move_Number_None, 0 },
        { "- 1", Move_Number_None, 0 },
        { "+-1", Move_Number_None, 0 },
        { ".5", Move_Number_None, 0 },
        { "\x01" "1", Move_Number_None, 0 },
        { "\x1c" "1", Move_Number_None, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int value = 12345;
        move_number result = aae_movement_stoi(cases[i].text, &value);
        CHECK(result == cases[i].result);
        CHECK(value == (result == Move_Number_Ok ? cases[i].value : 12345));
        if (result != cases[i].result || (result == Move_Number_Ok && value != cases[i].value))
            printf("  stoi(\"%s\") gave %d, %d\n", cases[i].text, (int)result, value);
    }
}

// T7Overcharged's names, written out here as its code has them
static const char *const t7o_names[Move_Patch_Count] = { "slide_preserve_momentum", "jumpslowdown", "fast_sprintleap",
                                                          "fast_sprintlerpin", "slideslowdown", "canslidethensprint",
                                                          "instant_slide", "fastswap", "fastads", "fastjump",
                                                          "slideangles" };

static void test_names(void) {
    for (int patch = 0; patch < Move_Patch_Count; patch++) {
        CHECK(aae_movement_patch_named(t7o_names[patch]) == patch);
        CHECK(strcmp(aae_movement_patch_name((move_patch)patch), t7o_names[patch]) == 0);
    }
    CHECK(Move_Patch_Count == 11);
    CHECK(strcmp(aae_movement_patch_name(Move_Patch_Count), "?") == 0);
    CHECK(strcmp(aae_movement_patch_name((move_patch)-1), "?") == 0);
    // lower-cased first, A to Z only
    CHECK(aae_movement_patch_named("FastAds") == Move_FastAds);
    CHECK(aae_movement_patch_named("JUMPSLOWDOWN") == Move_JumpSlowdown);
    CHECK(aae_movement_patch_named("Slide_Preserve_Momentum") == Move_SlidePreserveMomentum);
    CHECK(aae_movement_patch_named("fastads ") == -1);
    CHECK(aae_movement_patch_named(" fastads") == -1);
    CHECK(aae_movement_patch_named("fastad") == -1);
    CHECK(aae_movement_patch_named("fastadss") == -1);
    CHECK(aae_movement_patch_named("fast") == -1);
    CHECK(aae_movement_patch_named("fast_ads") == -1);
    CHECK(aae_movement_patch_named("") == -1);
    CHECK(aae_movement_patch_named("all") == -1);
    CHECK(aae_movement_patch_named("slide_preserve_momentum_and_then_some_more_letters") == -1);
    CHECK(aae_movement_patch_named("fastads\xc3\x9f") == -1);
}

static void test_commands(void) {
    move_patch patch = Move_Patch_Count;
    bool on = false;
    const char *on_words[] = { "movement_patch", "fastads", "1" };
    CHECK(aae_movement_patch_command(3, on_words, &patch, &on) && patch == Move_FastAds && on);
    const char *off_words[] = { "movement_patch", "fastjump", "0" };
    CHECK(aae_movement_patch_command(3, off_words, &patch, &on) && patch == Move_FastJump && !on);
    // any number but 0 is on
    static const char *const numbers[] = { "-1", "2", "1x", "+1", " 3", "2147483647", "-2147483648" };
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        const char *words[] = { "movement_patch", "slideangles", numbers[i] };
        on = false;
        CHECK(aae_movement_patch_command(3, words, &patch, &on) && patch == Move_SlideAngles && on);
    }
    static const char *const zeros[] = { "0", "-0", "0x1", " 0", "00", "0.9" };
    for (size_t i = 0; i < sizeof(zeros) / sizeof(zeros[0]); i++) {
        const char *words[] = { "movement_patch", "instant_slide", zeros[i] };
        on = true;
        CHECK(aae_movement_patch_command(3, words, &patch, &on) && patch == Move_InstantSlide && !on);
    }
    const char *upper[] = { "movement_patch", "FASTJUMP", "1" };
    CHECK(aae_movement_patch_command(3, upper, &patch, &on) && patch == Move_FastJump && on);
    // nothing: not three words, a number std::stoi throws for, no such patch
    patch = Move_Patch_Count;
    on = false;
    const char *two[] = { "movement_patch", "fastads" };
    CHECK(!aae_movement_patch_command(2, two, &patch, &on));
    const char *four[] = { "movement_patch", "fastads", "1", "1" };
    CHECK(!aae_movement_patch_command(4, four, &patch, &on));
    const char *one[] = { "movement_patch" };
    CHECK(!aae_movement_patch_command(1, one, &patch, &on));
    CHECK(!aae_movement_patch_command(3, NULL, &patch, &on));
    static const char *const bad[] = { "x", "", "on", "99999999999", "-2147483649", "+" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const char *words[] = { "movement_patch", "fastads", bad[i] };
        CHECK(!aae_movement_patch_command(3, words, &patch, &on));
    }
    const char *nosuch[] = { "movement_patch", "fastreload", "1" };
    CHECK(!aae_movement_patch_command(3, nosuch, &patch, &on));
    const char *all[] = { "movement_patch", "all", "1" };
    CHECK(!aae_movement_patch_command(3, all, &patch, &on));
    CHECK(patch == Move_Patch_Count && !on);

    // movement_patch_all
    const char *all_on[] = { "movement_patch_all", "1" };
    CHECK(aae_movement_patch_all_command(2, all_on, &on) == Move_All_Switch && on);
    const char *all_off[] = { "movement_patch_all", "0" };
    CHECK(aae_movement_patch_all_command(2, all_off, &on) == Move_All_Switch && !on);
    const char *all_minus[] = { "movement_patch_all", "-5" };
    CHECK(aae_movement_patch_all_command(2, all_minus, &on) == Move_All_Switch && on);
    const char *all_hex[] = { "movement_patch_all", "0x1" };
    CHECK(aae_movement_patch_all_command(2, all_hex, &on) == Move_All_Switch && !on);
    on = true;
    const char *all_bad[] = { "movement_patch_all", "x" };
    CHECK(aae_movement_patch_all_command(2, all_bad, &on) == Move_All_Nothing && on);
    const char *all_big[] = { "movement_patch_all", "2147483648" };
    CHECK(aae_movement_patch_all_command(2, all_big, &on) == Move_All_Nothing && on);
    const char *all_one[] = { "movement_patch_all" };
    CHECK(aae_movement_patch_all_command(1, all_one, &on) == Move_All_Usage);
    const char *all_three[] = { "movement_patch_all", "1", "1" };
    CHECK(aae_movement_patch_all_command(3, all_three, &on) == Move_All_Usage);
    CHECK(aae_movement_patch_all_command(0, all_three, &on) == Move_All_Usage);
}

static void test_strafe_rule(void) {
    CHECK(float_bits(0.667f) == 0x3f2ac083 && float_bits(Strafe_Least) == 0x3f2ac083);
    CHECK(same_float(aae_movement_strafe_rule(true, 0.9f), 0.9f));
    CHECK(same_float(aae_movement_strafe_rule(true, 0.97f), 0.97f));
    CHECK(same_float(aae_movement_strafe_rule(true, 0.667f), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, nextafterf(0.667f, 1.0f)), nextafterf(0.667f, 1.0f)));
    CHECK(same_float(aae_movement_strafe_rule(true, nextafterf(0.667f, 0.0f)), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, 0.5f), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, -1.0f), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, 0.0f), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, NAN), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, -NAN), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, INFINITY), INFINITY));
    CHECK(same_float(aae_movement_strafe_rule(true, -INFINITY), 0.667f));
    CHECK(same_float(aae_movement_strafe_rule(true, 5.0f), 5.0f));
    CHECK(same_float(aae_movement_strafe_rule(false, 0.9f), 0.667f));
}

// ---- the game's side

static void test_init(void) {
    reset();
    // the commands are there whatever the setting says
    game_command_count = 0;
    aae_movement_init(0x100000000, 0);
    CHECK(!aae_movement_enabled() && !forced && plugin_base == 0x100000000 && made_up_base == 0x100000000);
    CHECK(game_command_count == 2);
    CHECK(strcmp(game_commands[0].name, "movement_patch") == 0 && game_commands[0].function == movement_patch_f &&
          game_commands[0].command == patch_command);
    CHECK(strcmp(game_commands[1].name, "movement_patch_all") == 0 &&
          game_commands[1].function == movement_patch_all_f && game_commands[1].command == patch_all_command);
    // they do nothing then, and the mod's start registers nothing
    aae_movement_t7o_started();
    CHECK(game_registrations == 0 && !atomic_load(&started) && !aae_movement_started());
    RUN("movement_patch_all", "1");
    RUN("movement_patch", "fastads", "1");
    CHECK(switch_bits() == 0 && seen_count == 0 && only_on(0));
    printing_begin();
    debug = true;
    RUN("movement_patch_all");
    RUN("movement_patch_all", "1");
    debug = false;
    CHECK(strcmp(printing_end(), "") == 0);

    // a command the game has already isn't added again
    game_command_count = 0;
    game_has_command = "movement_patch";
    aae_movement_init(0x100000000, 1);
    CHECK(game_command_count == 1 && strcmp(game_commands[0].name, "movement_patch_all") == 0);
    game_command_count = 0;
    game_has_command = "movement_patch_all";
    aae_movement_init(0x100000000, 1);
    CHECK(game_command_count == 1 && strcmp(game_commands[0].name, "movement_patch") == 0);
    game_has_command = NULL;
    game_command_count = 0;
    aae_movement_init(0x100000000, 1);
    CHECK(game_command_count == 2 && aae_movement_enabled());
    aae_movement_init(0x100000000, 1);
    CHECK(game_command_count == 2);

    // BO3MACFIX_MOVEFORCE and BO3MACFIX_MOVEDEBUG
    setenv("BO3MACFIX_MOVEFORCE", "1", 1);
    printing_begin();
    aae_movement_init(0x100000000, 1);
    printing_end();
    CHECK(forced && !debug);
    aae_movement_init(0x100000000, 0);
    CHECK(!forced && !aae_movement_started());
    setenv("BO3MACFIX_MOVEFORCE", "0", 1);
    aae_movement_init(0x100000000, 1);
    CHECK(!forced);
    unsetenv("BO3MACFIX_MOVEFORCE");
    setenv("BO3MACFIX_MOVEDEBUG", "1", 1);
    printing_begin();
    aae_movement_init(0x100000000, 1);
    printing_end();
    CHECK(debug && !forced);
    setenv("BO3MACFIX_MOVEDEBUG", "0", 1);
    aae_movement_init(0x100000000, 1);
    CHECK(!debug);
    unsetenv("BO3MACFIX_MOVEDEBUG");

    // the real game's functions, at its base address
    game_functions(0x100000000);
    CHECK(Cmd_FindCommand == (void *)(0x100000000 + ADDR_Cmd_FindCommand) &&
          Cmd_AddCommandInternal == (void *)(0x100000000 + ADDR_Cmd_AddCommandInternal) &&
          Sys_GetTLS == (void *)(0x100000000 + ADDR_Sys_GetTLS) &&
          Dvar_RegisterInt == (void *)(0x100000000 + ADDR_Dvar_RegisterInt) &&
          Dvar_RegisterFloat == (void *)(0x100000000 + ADDR_Dvar_RegisterFloat) &&
          Dvar_RegisterBool == (void *)(0x100000000 + ADDR_Dvar_RegisterBool) &&
          Dvar_SetName == (void *)(0x100000000 + ADDR_Dvar_SetName) &&
          sniper_anim_types == (const int *)(0x100000000 + ADDR_ADS_SniperAnimTypes));
    CHECK(ADDR_Dvar_RegisterBool == 0xfa1328 && ADDR_Dvar_RegisterInt == 0xfa149e &&
          ADDR_Dvar_RegisterFloat == 0xfa1614 && ADDR_Cmd_FindCommand == 0xf764e8 &&
          ADDR_Cmd_AddCommandInternal == 0xf7651d && ADDR_Sys_GetTLS == 0xd0604f && ADDR_Dvar_SetName == 0xfa4242);
    CHECK(Dvar_Value == 0x28);
    made_up_game(0x100000000);
}

static void test_before_start(void) {
    reset();
    CHECK(aae_movement_enabled() && !aae_movement_started() && only_on(0) && !aae_movement_omni());
    CHECK(same_float(aae_movement_strafe_scale(), 0.667f) && same_float(aae_movement_fastads_multiplier(), 2.15f));
    // the commands change nothing before T7Overcharged's start (on Windows they aren't there yet)
    RUN("movement_patch_all", "1");
    RUN("movement_patch", "fastads", "1");
    CHECK(switch_bits() == 0 && seen_count == 0 && game_registrations == 0);
    printing_begin();
    debug = true;
    RUN("movement_patch_all", "1");
    debug = false;
    CHECK(strcmp(printing_end(), "movement: \"movement_patch_all 1\" does nothing: T7Overcharged's movement component "
                                 "hasn't started\n") == 0);
}

static void test_start(void) {
    // nobody set hook_movement_perks: T7Overcharged's default, 1
    reset();
    aae_movement_t7o_started();
    CHECK(atomic_load(&started) && aae_movement_started());
    game_dvar_t *perks = game_dvar("hook_movement_perks"), *fastads = game_dvar("t7o_perk_fastads_multiplier"),
                *strafe = game_dvar("t7o_strafe_speed_scale"), *omni = game_dvar("t7o_omnimovement");
    CHECK(perks != NULL && fastads != NULL && strafe != NULL && omni != NULL && game_dvar_count == 4);
    if (perks == NULL || fastads == NULL || strafe == NULL || omni == NULL)
        return;
    // T7Overcharged's order, kinds, defaults, domains, flags and descriptions, and the names written in
    CHECK(perks->order == 1 && fastads->order == 2 && strafe->order == 3 && omni->order == 4);
    CHECK(perks->type == 6 && perks->int_default == 1 && perks->int_min == 0 && perks->int_max == 1 &&
          perks->flags == 0 && strcmp(perks->description, "enhance player movements") == 0);
    CHECK(fastads->type == 2 && same_float(fastads->value_default, 2.15f) && same_float(fastads->min, 1.0f) &&
          same_float(fastads->max, 3.0f) && fastads->flags == 0 &&
          strcmp(fastads->description, "fastads multiplier") == 0);
    CHECK(float_bits(fastads->value_default) == 0x4009999a);
    CHECK(strafe->type == 2 && float_bits(strafe->value_default) == 0x3f2ac083 &&
          float_bits(strafe->min) == 0x3f2ac083 && float_bits(strafe->max) == 0x3f7851ec && strafe->flags == 0 &&
          strcmp(strafe->description, "strafe speed scale") == 0);
    CHECK(omni->type == 1 && !omni->bool_default && omni->flags == 0 &&
          strcmp(omni->description, "enable omni-movement") == 0);
    CHECK(name_in("hook_movement_perks") != NULL && strcmp(name_in("hook_movement_perks"), "hook_movement_perks") == 0);
    CHECK(name_in("t7o_perk_fastads_multiplier") != NULL &&
          strcmp(name_in("t7o_perk_fastads_multiplier"), "t7o_perk_fastads_multiplier") == 0);
    CHECK(name_in("t7o_strafe_speed_scale") != NULL &&
          strcmp(name_in("t7o_strafe_speed_scale"), "t7o_strafe_speed_scale") == 0);
    CHECK(name_in("t7o_omnimovement") != NULL && strcmp(name_in("t7o_omnimovement"), "t7o_omnimovement") == 0);
    // through the game's own function, so that +8 holds the game's own copy, the one its shutdown releases
    CHECK(game_namings == 4 && game_names_released == 0);
    CHECK(game_string(name_in("hook_movement_perks")) && game_string(name_in("t7o_perk_fastads_multiplier")) &&
          game_string(name_in("t7o_strafe_speed_scale")) && game_string(name_in("t7o_omnimovement")));
    // all off, then fastswap off once more: they were off
    CHECK(only_on(0) && seen_count == Move_Patch_Count + 1);
    for (int i = 0; i < seen_count && i < 64; i++)
        CHECK(!seen[i].on && !seen[i].changed);
    CHECK(seen[Move_Patch_Count].patch == Move_FastSwap);
    // the dvars as registered: omni off, strafe 0.667, the multiplier still the default
    CHECK(!aae_movement_omni() && same_float(aae_movement_strafe_scale(), 0.667f) &&
          same_float(aae_movement_fastads_multiplier(), 2.15f));
    // once per launch: the mod's loader runs again at every map
    aae_movement_t7o_started();
    CHECK(perks->registrations == 1 && fastads->registrations == 1 && strafe->registrations == 1 &&
          omni->registrations == 1 && seen_count == Move_Patch_Count + 1);

    // hook_movement_perks 0 when it starts: nothing else registered, and so it stays for this launch
    reset();
    set_before("hook_movement_perks", "0");
    aae_movement_t7o_started();
    CHECK(!atomic_load(&started) && !aae_movement_started() && game_registrations == 1 && game_dvar_count == 1 &&
          seen_count == 0 && game_namings == 1);
    CHECK(int_of("hook_movement_perks") == 0);
    RUN("movement_patch_all", "1");
    CHECK(only_on(0) && switch_bits() == 0);
    set_before("hook_movement_perks", "1");
    aae_movement_t7o_started();
    CHECK(!atomic_load(&started) && game_registrations == 1);
    // the game puts the mod's text inside the domain: 5 is 1, text that isn't a number is 0
    reset();
    set_before("hook_movement_perks", "5");
    aae_movement_t7o_started();
    CHECK(atomic_load(&started) && int_of("hook_movement_perks") == 1);
    reset();
    set_before("hook_movement_perks", "yes");
    aae_movement_t7o_started();
    CHECK(!atomic_load(&started));
    reset();
    set_before("hook_movement_perks", "-1");
    aae_movement_t7o_started();
    CHECK(!atomic_load(&started) && int_of("hook_movement_perks") == 0);
    // a hook_movement_perks registered before with another domain keeps its value (the game re-registers only text
    // dvars): T7Overcharged starts only for 1
    reset();
    game_register_int(gsc_hash("hook_movement_perks"), "hook_movement_perks", 2, 0, 5, 0, "someone else's");
    aae_movement_t7o_started();
    CHECK(!atomic_load(&started) && int_of("hook_movement_perks") == 2);
    // no dvar from the game: not started
    reset();
    game_refuses_registration = true;
    aae_movement_t7o_started();
    CHECK(!atomic_load(&started) && !aae_movement_started() && game_namings == 0);
    // the setting off: nothing registered
    reset();
    enabled = false;
    aae_movement_t7o_started();
    CHECK(game_registrations == 0 && !atomic_load(&started));
}

// All-around Enhancement's loader, as conditions.lua runs it at every UI start
static void mod_loader(void) {
    mod_sets("t7o_perk_fastads_multiplier", "1.3999999761581");
    mod_sets("hook_movement_perks", "1");
    aae_movement_t7o_started();
    mod_sets("t7o_omnimovement", "1");
    mod_sets("t7o_strafe_speed_scale", "0.89999997615814");
    RUN("movement_patch_all", "1");
    RUN("movement_patch", "jumpslowdown", "1");
    RUN("movement_patch", "fastswap", "0");
}

static void test_mod_loader(void) {
    reset();
    mod_loader();
    CHECK(aae_movement_started() && only_on(ALL_PATCHES & ~(1u << Move_FastSwap)));
    CHECK(aae_movement_omni() && same_float(aae_movement_strafe_scale(), 0.9f));
    CHECK(same_float(aae_movement_fastads_multiplier(), 1.4f) && same_float(float_of("t7o_perk_fastads_multiplier"),
                                                                              1.4f));
    // the start's twelve, then movement_patch_all in T7Overcharged's order (jumpslowdown last), then jumpslowdown
    // (on already) and fastswap off
    static const char *const all_order[Move_Patch_Count] = { "slide_preserve_momentum", "fast_sprintleap",
                                                             "fast_sprintlerpin",       "slideslowdown",
                                                             "canslidethensprint",      "instant_slide",
                                                             "fastswap",                "fastads",
                                                             "fastjump",                "slideangles",
                                                             "jumpslowdown" };
    CHECK(seen_count == 12 + 11 + 2);
    for (int i = 0; i < Move_Patch_Count && i < 64; i++) {
        CHECK(strcmp(t7o_names[seen[i].patch], all_order[i]) == 0 && !seen[i].on);
        CHECK(strcmp(t7o_names[seen[12 + i].patch], all_order[i]) == 0 && seen[12 + i].on && seen[12 + i].changed);
    }
    CHECK(seen[23].patch == Move_JumpSlowdown && seen[23].on && !seen[23].changed);
    CHECK(seen[24].patch == Move_FastSwap && !seen[24].on && seen[24].changed);
    CHECK(seen[12 + 7].patch == Move_FastAds && same_float(seen[12 + 7].multiplier, 1.4f) &&
          same_float(seen[12 + 7].before, 2.15f));
    for (int i = 0; i < Move_Patch_Count; i++)
        CHECK(aae_movement_all_order[i] == (move_patch)aae_movement_patch_named(all_order[i]));

    // the next map: the loader again; T7Overcharged doesn't start again, fastads copies the multiplier again
    seen_count = 0;
    mod_loader();
    CHECK(game_dvar("hook_movement_perks")->registrations == 1 && seen_count == 11 + 2);
    CHECK(only_on(ALL_PATCHES & ~(1u << Move_FastSwap)) && same_float(aae_movement_fastads_multiplier(), 1.4f));
    CHECK(seen[7].patch == Move_FastAds && seen[7].on && !seen[7].changed && same_float(seen[7].multiplier, 1.4f));
    CHECK(seen[6].patch == Move_FastSwap && seen[6].on && seen[6].changed);

    // the mod's HUD when the server says no_movement_patch, and the snail option
    RUN("movement_patch_all", "0");
    CHECK(only_on(0) && aae_movement_started());
    RUN("movement_patch_all", "1");
    RUN("movement_patch", "jumpslowdown", "0");
    CHECK(only_on(ALL_PATCHES & ~(1u << Move_JumpSlowdown)));
    RUN("movement_patch", "JumpSlowdown", "1");
    CHECK(only_on(ALL_PATCHES));
    // what changes nothing
    uint32_t before = switch_bits();
    printing_begin();
    RUN("movement_patch", "fastads", "x");
    RUN("movement_patch", "nosuch", "0");
    RUN("movement_patch", "fastads");
    RUN("movement_patch", "fastads", "0", "0");
    RUN("movement_patch_all", "off");
    RUN("movement_patch_all", "0", "0");
    RUN("movement_patch_all");
    CHECK(strcmp(printing_end(), "movement_patch_all <0/1>\nmovement_patch_all <0/1>\n") == 0);
    CHECK(switch_bits() == before);
    RUN("movement_patch", "fastads", "-1");
    CHECK(aae_movement_patch_on(Move_FastAds));
    RUN("movement_patch", "fastads", "0x5");
    CHECK(!aae_movement_patch_on(Move_FastAds));
}

static void test_multiplier(void) {
    reset();
    aae_movement_t7o_started();
    // copied when switched on, from the dvar as it is then
    RUN("movement_patch", "fastads", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 2.15f));
    CHECK(set_float("t7o_perk_fastads_multiplier", 1.4f));
    CHECK(same_float(aae_movement_fastads_multiplier(), 2.15f));
    // switched on again while on: copied again
    RUN("movement_patch", "fastads", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 1.4f));
    // the game refuses a value outside the domain, so it stays
    CHECK(!set_float("t7o_perk_fastads_multiplier", 5.0f) && !set_float("t7o_perk_fastads_multiplier", 0.5f));
    RUN("movement_patch", "fastads", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 1.4f));
    // off: nothing copied; the multiplier only counts while fastads is on
    CHECK(set_float("t7o_perk_fastads_multiplier", 3.0f));
    RUN("movement_patch", "fastads", "0");
    CHECK(!aae_movement_patch_on(Move_FastAds) && same_float(aae_movement_fastads_multiplier(), 1.4f));
    RUN("movement_patch_all", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 3.0f));
    CHECK(set_float("t7o_perk_fastads_multiplier", 1.0f));
    RUN("movement_patch_all", "0");
    CHECK(same_float(aae_movement_fastads_multiplier(), 3.0f));
    RUN("movement_patch_all", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 1.0f));
    // another patch switched on copies nothing
    CHECK(set_float("t7o_perk_fastads_multiplier", 2.5f));
    RUN("movement_patch", "fastjump", "1");
    RUN("movement_patch", "slideangles", "0");
    CHECK(same_float(aae_movement_fastads_multiplier(), 1.0f));
    // its four bytes as they are
    put_float("t7o_perk_fastads_multiplier", 1.2345678f);
    RUN("movement_patch", "fastads", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 1.2345678f));
    uint32_t odd = 0x7fc12345;
    memcpy(game_dvar("t7o_perk_fastads_multiplier")->game_side + 0x28, &odd, sizeof(odd));
    RUN("movement_patch", "fastads", "1");
    CHECK(float_bits(aae_movement_fastads_multiplier()) == odd);
    // the game gave no float dvars: switching fastads on keeps the multiplier, and the strafe scale is the game's
    reset();
    game_refuses_floats = true;
    aae_movement_t7o_started();
    CHECK(atomic_load(&started) && atomic_load(&fastads_dvar) == NULL && atomic_load(&strafe_dvar) == NULL);
    RUN("movement_patch", "fastads", "1");
    CHECK(aae_movement_patch_on(Move_FastAds) && same_float(aae_movement_fastads_multiplier(), 2.15f));
    CHECK(same_float(aae_movement_strafe_scale(), 0.667f));
    // what the mod set before T7Overcharged loaded: the game turns the text into a number inside 1 to 3
    reset();
    set_before("t7o_perk_fastads_multiplier", "1.4");
    aae_movement_t7o_started();
    RUN("movement_patch", "fastads", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 1.4f));
    reset();
    set_before("t7o_perk_fastads_multiplier", "7");
    aae_movement_t7o_started();
    RUN("movement_patch_all", "1");
    CHECK(same_float(aae_movement_fastads_multiplier(), 3.0f));
}

static void test_live_dvars(void) {
    reset();
    aae_movement_t7o_started();
    CHECK(!aae_movement_omni());
    set_bool("t7o_omnimovement", true);
    CHECK(aae_movement_omni());
    set_bool("t7o_omnimovement", false);
    CHECK(!aae_movement_omni());
    CHECK(set_float("t7o_strafe_speed_scale", 0.9f) && same_float(aae_movement_strafe_scale(), 0.9f));
    CHECK(set_float("t7o_strafe_speed_scale", 0.7f) && same_float(aae_movement_strafe_scale(), 0.7f));
    CHECK(!set_float("t7o_strafe_speed_scale", 0.98f) && same_float(aae_movement_strafe_scale(), 0.7f));
    put_float("t7o_strafe_speed_scale", 0.5f);
    CHECK(same_float(aae_movement_strafe_scale(), 0.667f));
    put_float("t7o_strafe_speed_scale", NAN);
    CHECK(same_float(aae_movement_strafe_scale(), 0.667f));
    put_float("t7o_strafe_speed_scale", 0.95f);
    CHECK(same_float(aae_movement_strafe_scale(), 0.95f));
    // the strafe scale and omni-movement don't wait for a patch: they are no patch
    CHECK(only_on(0));
    // what the mod set before T7Overcharged loaded, and the game's domain at registration
    reset();
    set_before("t7o_strafe_speed_scale", "0.9");
    set_before("t7o_omnimovement", "1");
    aae_movement_t7o_started();
    CHECK(same_float(aae_movement_strafe_scale(), 0.9f) && aae_movement_omni());
    reset();
    set_before("t7o_strafe_speed_scale", "2");
    aae_movement_t7o_started();
    CHECK(same_float(aae_movement_strafe_scale(), 0.97f));
}

static void test_mod_gate(void) {
    reset();
    mod_loader();
    mod_loaded = false;
    CHECK(!aae_movement_started() && only_on(0) && !aae_movement_omni());
    CHECK(same_float(aae_movement_strafe_scale(), 0.667f));
    // the switches stay as the commands left them (T7Overcharged stays in the game on Windows)
    CHECK(switch_bits() == (ALL_PATCHES & ~(1u << Move_FastSwap)));
    mod_loaded = true;
    CHECK(aae_movement_started() && only_on(ALL_PATCHES & ~(1u << Move_FastSwap)) && aae_movement_omni());
    CHECK(!aae_movement_patch_on(Move_Patch_Count) && !aae_movement_patch_on((move_patch)-1) &&
          !aae_movement_patch_on((move_patch)32));
}

static void test_forced(void) {
    reset();
    forced = true;
    CHECK(aae_movement_started() && only_on(ALL_PATCHES & ~(1u << Move_FastSwap)) && aae_movement_omni());
    CHECK(same_float(aae_movement_strafe_scale(), 0.9f) && same_float(aae_movement_fastads_multiplier(), 1.4f));
    mod_loaded = false;
    CHECK(aae_movement_started() && aae_movement_patch_on(Move_FastJump) && aae_movement_omni());
    CHECK(!aae_movement_patch_on(Move_Patch_Count));
    // whatever the commands do
    mod_loaded = true;
    aae_movement_t7o_started();
    RUN("movement_patch_all", "0");
    RUN("movement_patch", "fastswap", "1");
    CHECK(only_on(ALL_PATCHES & ~(1u << Move_FastSwap)) && same_float(aae_movement_strafe_scale(), 0.9f));
    forced = false;
    CHECK(only_on(1u << Move_FastSwap) && !aae_movement_omni() && same_float(aae_movement_strafe_scale(), 0.667f));
}

static void test_words(void) {
    reset();
    aae_movement_t7o_started();
    // the words of the level the command runs at
    const char *words[] = { "movement_patch", "fastjump", "1" };
    run_at(3, ARGC(words), words, "movement_patch");
    CHECK(only_on(1u << Move_FastJump));
    const char *other[] = { "movement_patch_all", "1" };
    run_at(7, ARGC(other), other, "movement_patch_all");
    CHECK(only_on(ALL_PATCHES));
    // a level outside the game's eight, or no arguments: nothing
    const char *off[] = { "movement_patch_all", "0" };
    run_at(8, ARGC(off), off, "movement_patch_all");
    run_at(-1, ARGC(off), off, "movement_patch_all");
    CHECK(only_on(ALL_PATCHES));
    memset(game_args, 0, sizeof(game_args));
    void *none = NULL;
    memcpy(game_tls + 0x18, &none, sizeof(none));
    movement_patch_all_f();
    no_tls = true;
    movement_patch_all_f();
    movement_patch_f();
    no_tls = false;
    CHECK(only_on(ALL_PATCHES));
    void *args = game_args;
    memcpy(game_tls + 0x18, &args, sizeof(args));
    run_at(0, 2, NULL, "movement_patch_all");
    CHECK(only_on(ALL_PATCHES));
    run_at(0, ARGC(off), off, "movement_patch_all");
    CHECK(only_on(0));
}

static void test_printed(void) {
    reset();
    switched = print_switch;
    // the usage line, whatever the debug switch says; nothing else without it
    printing_begin();
    aae_movement_t7o_started();
    RUN("movement_patch_all");
    RUN("movement_patch_all", "1", "2");
    RUN("movement_patch_all", "1");
    RUN("movement_patch", "fastads");
    CHECK(strcmp(printing_end(), "movement_patch_all <0/1>\nmovement_patch_all <0/1>\n") == 0);
    // with BO3MACFIX_MOVEDEBUG=1: the start, each switch that changes, what is on after each command
    reset();
    switched = print_switch;
    debug = true;
    printing_begin();
    set_before("t7o_perk_fastads_multiplier", "1.4");
    aae_movement_t7o_started();
    set_bool("t7o_omnimovement", true);
    set_float("t7o_strafe_speed_scale", 0.9f);
    RUN("movement_patch_all", "1");
    RUN("movement_patch", "jumpslowdown", "1");
    RUN("movement_patch", "fastswap", "0");
    RUN("movement_patch", "fastads", "1");
    set_float("t7o_perk_fastads_multiplier", 2.0f);
    RUN("movement_patch", "FastAds", "1");
    put_float("t7o_strafe_speed_scale", 0.5f);
    RUN("movement_patch", "fastswap", "x");
    const char *expected =
        "movement: the mod started T7Overcharged: hook_movement_perks is 1, its movement component is on: "
        "t7o_perk_fastads_multiplier 1.4, t7o_strafe_speed_scale 0.667, t7o_omnimovement 0; every patch off\n"
        "movement: slide_preserve_momentum on\n"
        "movement: fast_sprintleap on\n"
        "movement: fast_sprintlerpin on\n"
        "movement: slideslowdown on\n"
        "movement: canslidethensprint on\n"
        "movement: instant_slide on\n"
        "movement: fastswap on\n"
        "movement: fastads on, fast ADS multiplier 1.4\n"
        "movement: fastjump on\n"
        "movement: slideangles on\n"
        "movement: jumpslowdown on\n"
        "movement: after \"movement_patch_all 1\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastswap fastads fastjump slideangles], "
        "off []; "
        "t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n"
        "movement: after \"movement_patch jumpslowdown 1\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastswap fastads fastjump slideangles], "
        "off []; "
        "t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n"
        "movement: fastswap off\n"
        "movement: after \"movement_patch fastswap 0\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastads fastjump slideangles], "
        "off [fastswap]; "
        "t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n"
        "movement: after \"movement_patch fastads 1\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastads fastjump slideangles], "
        "off [fastswap]; "
        "t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n"
        "movement: fastads on, fast ADS multiplier 2 (copied again)\n"
        "movement: after \"movement_patch FastAds 1\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastads fastjump slideangles], "
        "off [fastswap]; "
        "t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 2\n"
        "movement: after \"movement_patch fastswap x\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastads fastjump slideangles], "
        "off [fastswap]; t7o_omnimovement 1, t7o_strafe_speed_scale 0.5 (strafe scale 0.667), fast ADS multiplier 2\n";
    const char *out = printing_end();
    CHECK(strcmp(out, expected) == 0);
    if (strcmp(out, expected) != 0)
        printf("  printed:\n%s", out);
    // hook_movement_perks not 1
    reset();
    debug = true;
    set_before("hook_movement_perks", "0");
    printing_begin();
    aae_movement_t7o_started();
    CHECK(strcmp(printing_end(), "movement: the mod started T7Overcharged: hook_movement_perks is 0, its movement "
                                 "component stays off\n") == 0);
    // init's lines
    reset();
    setenv("BO3MACFIX_MOVEDEBUG", "1", 1);
    printing_begin();
    aae_movement_init(0x100000000, 0);
    aae_movement_init(0x100000000, 1);
    setenv("BO3MACFIX_MOVEFORCE", "1", 1);
    aae_movement_init(0x100000000, 1);
    unsetenv("BO3MACFIX_MOVEDEBUG");
    aae_movement_init(0x100000000, 1);
    unsetenv("BO3MACFIX_MOVEFORCE");
    aae_movement_init(0x100000000, 0);
    aae_movement_init(0x100000000, 1);
    const char *forced_line = "movement: BO3MACFIX_MOVEFORCE=1: every change on as the mod leaves it in an ordinary "
                              "game (all patches but fastswap, omni-movement, strafe scale 0.9, fast ADS multiplier "
                              "1.4), whatever its switches say\n";
    const char *detours_line = "movement: fast ADS: the two detours are in (the perk test, the doubling)\n";
    char init_expected[2048];
    snprintf(init_expected, sizeof(init_expected), "%s%s%s%s%s%s",
             "movement: AAEMovement is 0: T7Overcharged's movement changes stay off; movement_patch and "
             "movement_patch_all are there and do nothing\n",
             "movement: AAEMovement is 1: the switches wait for the mod to start T7Overcharged\n", detours_line,
             forced_line, detours_line, forced_line);
    out = printing_end();
    CHECK(strcmp(out, init_expected) == 0);
    if (strcmp(out, init_expected) != 0)
        printf("  printed:\n%s", out);
}

// The movement code reads on two threads while a command switches: never a multiplier that wasn't one, and never
// fastads on with the multiplier from before its first switch-on.
static _Atomic bool reading;
static _Atomic int bad_reads;
static void *reader(void *unused) {
    (void)unused;
    while (atomic_load(&reading)) {
        bool on = aae_movement_patch_on(Move_FastAds);
        float multiplier = aae_movement_fastads_multiplier();
        if (!same_float(multiplier, 1.0f) && !same_float(multiplier, 3.0f) && !same_float(multiplier, 2.15f))
            atomic_fetch_add(&bad_reads, 1);
        if (on && same_float(multiplier, 2.15f))
            atomic_fetch_add(&bad_reads, 1);
        (void)aae_movement_omni();
        (void)aae_movement_strafe_scale();
        (void)aae_movement_started();
    }
    return NULL;
}
static void test_threads(void) {
    reset();
    aae_movement_t7o_started();
    atomic_store(&reading, true);
    pthread_t threads[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, reader, NULL);
    for (int i = 0; i < 20000; i++) {
        set_float("t7o_perk_fastads_multiplier", i % 2 != 0 ? 3.0f : 1.0f);
        RUN("movement_patch", "fastads", i % 3 != 0 ? "1" : "0");
        if (i % 100 == 0)
            RUN("movement_patch_all", i % 200 != 0 ? "1" : "0");
    }
    atomic_store(&reading, false);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);
    CHECK(atomic_load(&bad_reads) == 0);
}

// before anything else: the switches as they are when the game starts
static void test_first_state(void) {
    CHECK(switch_bits() == 0 && same_float(aae_movement_fastads_multiplier(), 2.15f));
    CHECK(float_bits(aae_movement_fastads_multiplier()) == 0x4009999a);
    CHECK(!aae_movement_enabled() && !aae_movement_started() && !aae_movement_omni());
}

// ---- fast ADS

// the game's doubling behind the detour, "addss xmm0, xmm0", on what the rule leaves for a rate
static float doubled(float rate, float multiplier) {
    float halved = aae_movement_fastads_halved(rate, multiplier);
    return halved + halved;
}
// Windows with T7Overcharged's patch: "mulss xmm6, [multiplier]"
static float windows(float rate, float multiplier) {
    return rate * multiplier;
}
static bool exact(float rate, float multiplier) {
    return float_bits(doubled(rate, multiplier)) == float_bits(windows(rate, multiplier));
}

static void test_fastads_rule(void) {
    // every ADS time of 1 to 2000 ms (the game's integer branch: 1 / (float)ms, negative when lowering) by multipliers
    // from 1 to 3 every 1/1024, T7Overcharged's default and AAE's, and the floats right around them
    static const float special[] = { 1.0f, 1.4f, 2.0f, 2.15f, 3.0f };
    int wrong = 0;
    long cases = 0;
    for (int ms = 1; ms <= 2000; ms++) {
        for (int sign = 0; sign < 2; sign++) {
            float rate = (sign ? -1.0f : 1.0f) / (float)ms;
            for (int step = 0; step <= 2048; step++, cases++)
                wrong += !exact(rate, 1.0f + (float)step / 1024.0f);
            for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); i++, cases += 3) {
                wrong += !exact(rate, special[i]);
                wrong += !exact(rate, nextafterf(special[i], 0.0f)) + !exact(rate, nextafterf(special[i], 4.0f));
            }
        }
    }
    CHECK(wrong == 0 && cases == 2000 * 2 * (2049 + 15));
    // every float multiplier from 1 to 3 with a few rates of the sights
    cases = 0;
    static const float rates[] = { 1.0f / 300.0f, -1.0f / 250.0f, 0.0033333334f, -0.0066666668f, 1.0f / 7.0f };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        for (float multiplier = 1.0f; multiplier <= 3.0f; multiplier = nextafterf(multiplier, 4.0f), cases++)
            wrong += !exact(rates[i], multiplier);
    CHECK(wrong == 0 && cases == 5L * ((1L << 23) + (1L << 22) + 1));
    cases = 0;
    // the float branches' rates can be anything: every exponent from -125 to 127, mantissas from a fixed generator
    uint32_t seed = 12345;
    for (int exponent = -125 + 2; exponent <= 127 - 2; exponent++) {
        for (int i = 0; i < 2000; i++) {
            seed = seed * 1103515245u + 12345u;
            float rate = ldexpf(1.0f + (float)(seed >> 9) / 8388608.0f, exponent - 2);
            seed = seed * 1103515245u + 12345u;
            float multiplier = 1.0f + 2.0f * (float)(seed >> 9) / 8388608.0f;
            wrong += !exact((seed & 1) ? -rate : rate, multiplier);
            cases++;
        }
    }
    CHECK(wrong == 0 && cases == 249L * 2000);
    // the edges: no ADS time (1 / 0), and the sights at rest
    CHECK(exact(INFINITY, 1.4f) && exact(-INFINITY, 1.4f) && exact(0.0f, 1.4f) && exact(-0.0f, 2.15f));
    CHECK(isnan(doubled(NAN, 1.4f)));
    CHECK(same_float(aae_movement_fastads_halved(1.0f / 300.0f, 1.4f), (1.0f / 300.0f * 1.4f) * 0.5f));
}

// the two values the perk test's setup puts together, as the thunk's assembly does: the player state with "fast" in
// its top byte, the weapon's definition with "going up" in its top bit
static uint64_t perk_first(const void *player_state, uint8_t fast) {
    return (uint64_t)(uintptr_t)player_state | (uint64_t)fast << 56;
}
static uint64_t perk_second(const void *weapon, bool up) {
    return (uint64_t)(uintptr_t)weapon | (uint64_t)up << 63;
}

static void test_fastads_detours(void) {
    // installed only with the setting on and the pre-patched executable's two stubs
    reset();
    CHECK(hooks == 2);
    CHECK(was_hooked(ADDR_ADS_FastAdsPerkTest, DETOUR_ENTRY(FastAds_PerkTest), &orig_FastAds_PerkTest) &&
          other_FastAds_PerkTest == (void *)(0x100000000 + ADDR_ADS_FastAdsTestsPassed));
    CHECK(was_hooked(ADDR_ADS_FastAdsDouble, DETOUR_ENTRY(FastAds_Double), &orig_FastAds_Double));
    CHECK(ADDR_ADS_FastAdsPerkTest == 0xed5e2b && ADDR_ADS_FastAdsTestsPassed == 0xed5e58 &&
          ADDR_ADS_FastAdsDouble == 0xed5e5c && ADDR_ADS_SniperAnimTypes == 0x4076854);
    hooks = 0;
    aae_movement_init(0x100000000, 0);
    CHECK(hooks == 0);
    printing_begin();
    have_stubs = false;
    aae_movement_init(0x100000000, 1);
    have_stubs = true;
    stub_missing_for = ADDR_ADS_FastAdsDouble;
    aae_movement_init(0x100000000, 1);
    stub_missing_for = ADDR_ADS_FastAdsPerkTest;
    aae_movement_init(0x100000000, 1);
    stub_missing_for = -1;
    const char *no_stubs = "movement: not running from the pre-patched executable: fast ADS stays the game's own\n";
    char three[512];
    snprintf(three, sizeof(three), "%s%s%s", no_stubs, no_stubs, no_stubs);
    CHECK(strcmp(printing_end(), three) == 0);
    CHECK(hooks == 0);

    // the perk test: past the sniper rifle tests only with the perk and fastads on (and without the debug switch
    // nothing of it is printed, which the end of this test checks)
    reset();
    printing_begin();
    uint8_t ps[0xc00] = { 0 }, weapon[0x80] = { 0 };
    float sights = 0.25f;
    memcpy(ps + 0x2fc, &sights, sizeof(sights));
    int anim_type = 5;
    memcpy(weapon + 0x68, &anim_type, sizeof(anim_type));
    uint64_t second = perk_second(weapon, true);
    CHECK(!handler_FastAds_PerkTest(perk_first(ps, 1), second)); // before the start
    mod_loader();
    CHECK(handler_FastAds_PerkTest(perk_first(ps, 1), second));
    CHECK(handler_FastAds_PerkTest(perk_first(ps, 0x80), second)); // any r15b but 0, as the game's "test" has it
    CHECK(handler_FastAds_PerkTest(perk_first(ps, 1), perk_second(weapon, false)));
    CHECK(!handler_FastAds_PerkTest(perk_first(ps, 0), second)); // no perk: the game's own, which skips
    RUN("movement_patch", "fastads", "0");
    CHECK(!handler_FastAds_PerkTest(perk_first(ps, 1), second)); // fastads off: the game's own tests
    RUN("movement_patch", "fastads", "1");
    mod_loaded = false;
    CHECK(!handler_FastAds_PerkTest(perk_first(ps, 1), second));
    mod_loaded = true;
    CHECK(handler_FastAds_PerkTest(perk_first(ps, 1), 0));
    // BO3MACFIX_MOVEFASTADSPERK=1: the player counts as having the perk, while fastads is on
    fastads_perk_forced = true;
    CHECK(handler_FastAds_PerkTest(perk_first(ps, 0), second));
    RUN("movement_patch", "fastads", "0");
    CHECK(!handler_FastAds_PerkTest(perk_first(ps, 0), second) && !handler_FastAds_PerkTest(perk_first(ps, 1), second));
    RUN("movement_patch", "fastads", "1");
    mod_loaded = false;
    CHECK(!handler_FastAds_PerkTest(perk_first(ps, 0), second));
    mod_loaded = true;
    fastads_perk_forced = false;
    // forced: AAE's state whatever the switches say
    forced = true;
    RUN("movement_patch_all", "0");
    CHECK(handler_FastAds_PerkTest(perk_first(ps, 1), second) && !handler_FastAds_PerkTest(perk_first(ps, 0), second));
    forced = false;
    RUN("movement_patch_all", "1");
    RUN("movement_patch", "fastswap", "0");

    // the doubling: the rate in xmm0 left so that the game's own doubling gives rate * the copied multiplier
    detour_registers_t registers, before;
    memset(&registers, 0x5a, sizeof(registers));
    registers.rbx = (uint64_t)(uintptr_t)ps;
    registers.r12 = (uint64_t)(uintptr_t)weapon;
    float rate = -1.0f / 300.0f;
    memcpy(registers.xmm[0], &rate, sizeof(rate));
    before = registers;
    handler_FastAds_Double(&registers);
    float left;
    memcpy(&left, registers.xmm[0], sizeof(left));
    CHECK(same_float(left + left, rate * 1.4f) && same_float(left, aae_movement_fastads_halved(rate, 1.4f)));
    CHECK(memcmp(registers.xmm[0] + 4, before.xmm[0] + 4, 12) == 0);
    CHECK(memcmp(registers.xmm[1], before.xmm[1], sizeof(registers) - 16) == 0);
    // the multiplier as copied: a later dvar value counts only after fastads is switched on again
    CHECK(set_float("t7o_perk_fastads_multiplier", 2.0f));
    registers = before;
    handler_FastAds_Double(&registers);
    memcpy(&left, registers.xmm[0], sizeof(left));
    CHECK(same_float(left + left, rate * 1.4f));
    RUN("movement_patch", "fastads", "1");
    registers = before;
    handler_FastAds_Double(&registers);
    memcpy(&left, registers.xmm[0], sizeof(left));
    CHECK(same_float(left + left, rate * 2.0f));
    // off, without the mod: the game's own rate, every register as it was (with a multiplier whose half isn't 1)
    CHECK(set_float("t7o_perk_fastads_multiplier", 1.4f));
    RUN("movement_patch", "fastads", "1");
    RUN("movement_patch_all", "0");
    registers = before;
    handler_FastAds_Double(&registers);
    CHECK(memcmp(&registers, &before, sizeof(registers)) == 0);
    RUN("movement_patch_all", "1");
    mod_loaded = false;
    handler_FastAds_Double(&registers);
    CHECK(memcmp(&registers, &before, sizeof(registers)) == 0);
    mod_loaded = true;
    // forced: AAE's 1.4 whatever the switches say
    forced = true;
    RUN("movement_patch_all", "0");
    registers = before;
    handler_FastAds_Double(&registers);
    memcpy(&left, registers.xmm[0], sizeof(left));
    CHECK(same_float(left + left, rate * 1.4f));
    forced = false;
    CHECK(strcmp(printing_end(), "") == 0);
}

// the lines: each place's first time on a thread, the perk test when what it saw changes, the doubling's sums
static uint8_t line_ps[0xc00], line_weapon[0x80];
static char elsewhere_thread[32];
static void *perk_test_elsewhere(void *unused) {
    (void)unused;
    handler_FastAds_PerkTest(perk_first(line_ps, 0), perk_second(line_weapon, true));
    snprintf(elsewhere_thread, sizeof(elsewhere_thread), "%u", (unsigned)pthread_mach_thread_np(pthread_self()));
    return NULL;
}
static void set_perks(uint32_t perks0, uint32_t script_perks0) {
    memcpy(line_ps + 0xb04, &perks0, sizeof(perks0));
    memcpy(line_ps + 0xaf4, &script_perks0, sizeof(script_perks0));
}
static void test_fastads_lines(void) {
    const uint32_t fast_ads = 1u << 23, fast_reload = 1u << 28;
    memset(line_ps, 0, sizeof(line_ps));
    uint32_t high = 0x80000001;
    memcpy(line_ps + 0xb04 + 12, &high, sizeof(high));
    float sights = 0.5f;
    memcpy(line_ps + 0x2fc, &sights, sizeof(sights));
    int anim_type = 7; // one of the made-up game's two sniper anim types
    memcpy(line_weapon + 0x68, &anim_type, sizeof(anim_type));
    reset();
    mod_loader();
    switched = print_switch;
    debug = true;
    printing_begin();
    set_perks(fast_reload, fast_reload);
    handler_FastAds_PerkTest(perk_first(line_ps, 0), perk_second(line_weapon, true));
    handler_FastAds_PerkTest(perk_first(line_ps, 0), perk_second(line_weapon, true)); // the same: no line
    handler_FastAds_PerkTest(perk_first(line_ps, 0), perk_second(line_weapon, false));
    set_perks(fast_ads | fast_reload, fast_ads | fast_reload);
    handler_FastAds_PerkTest(perk_first(line_ps, 1), perk_second(line_weapon, true));
    RUN("movement_patch", "fastads", "0");
    handler_FastAds_PerkTest(perk_first(line_ps, 1), perk_second(line_weapon, true));
    RUN("movement_patch", "fastads", "1");
    set_perks(0, 0);
    fastads_perk_forced = true;
    handler_FastAds_PerkTest(perk_first(line_ps, 0), perk_second(line_weapon, true));
    RUN("movement_patch", "fastads", "0");
    handler_FastAds_PerkTest(perk_first(line_ps, 0), perk_second(line_weapon, true));
    RUN("movement_patch", "fastads", "1");
    fastads_perk_forced = false;
    // the doubling, on and off
    detour_registers_t registers;
    memset(&registers, 0x5a, sizeof(registers));
    registers.rbx = (uint64_t)(uintptr_t)line_ps;
    registers.r12 = (uint64_t)(uintptr_t)line_weapon;
    float rate = -1.0f / 300.0f;
    memcpy(registers.xmm[0], &rate, sizeof(rate));
    detour_registers_t before = registers;
    handler_FastAds_Double(&registers);
    registers = before;
    handler_FastAds_Double(&registers); // the same: no line
    RUN("movement_patch", "fastads", "0");
    registers = before;
    handler_FastAds_Double(&registers);
    pthread_t thread;
    pthread_create(&thread, NULL, perk_test_elsewhere, NULL);
    pthread_join(thread, NULL);
    debug = false;
    const char *out = printing_end();

    unsigned me = pthread_mach_thread_np(pthread_self());
    unsigned long long ps = (unsigned long long)(uintptr_t)line_ps;
    char expected[8192] = "";
#define ADD(...) snprintf(expected + strlen(expected), sizeof(expected) - strlen(expected), __VA_ARGS__)
    ADD("movement: fast ADS: the perk test (0x100ed5e2b) reached for the first time on thread %u (player state "
        "0x%llx)\n", me, ps);
    const char *middle = "weapon anim type 7, sniper rifles 7 and 8): perks";
    ADD("movement: fast ADS: the perk test says no (thread %u, player state 0x%llx, sights going up, at 0.5, %s "
        "10000000 00000000 00000000 80000001 (specialty_fastads 0, specialty_fastreload 1), given by scripts 10000000 "
        "00000000 00000000 00000000; the game skips the speed-up\n", me, ps, middle);
    ADD("movement: fast ADS: the perk test says no (thread %u, player state 0x%llx, sights going down, at 0.5, %s "
        "10000000 00000000 00000000 80000001 (specialty_fastads 0, specialty_fastreload 1), given by scripts 10000000 "
        "00000000 00000000 00000000; the game skips the speed-up\n", me, ps, middle);
    ADD("movement: fast ADS: the perk test says yes (thread %u, player state 0x%llx, sights going up, at 0.5, %s "
        "10800000 00000000 00000000 80000001 (specialty_fastads 1, specialty_fastreload 1), given by scripts 10800000 "
        "00000000 00000000 00000000; fastads on: past the sniper rifle tests to the doubling\n", me, ps, middle);
    ADD("movement: fastads off\n");
    ADD("movement: after \"movement_patch fastads 0\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastjump slideangles], off [fastswap "
        "fastads]; t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n");
    ADD("movement: fast ADS: the perk test says yes (thread %u, player state 0x%llx, sights going up, at 0.5, %s "
        "10800000 00000000 00000000 80000001 (specialty_fastads 1, specialty_fastreload 1), given by scripts 10800000 "
        "00000000 00000000 00000000; fastads off: the game's own sniper rifle tests and doubling\n", me, ps, middle);
    ADD("movement: fastads on, fast ADS multiplier 1.4\n");
    ADD("movement: after \"movement_patch fastads 1\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastads fastjump slideangles], off "
        "[fastswap]; t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n");
    ADD("movement: fast ADS: the perk test says no (BO3MACFIX_MOVEFASTADSPERK: counted as yes) (thread %u, player "
        "state 0x%llx, sights going up, at 0.5, %s 00000000 00000000 00000000 80000001 (specialty_fastads 0, "
        "specialty_fastreload 0), given by scripts 00000000 00000000 00000000 00000000; fastads on: past the sniper "
        "rifle tests to the doubling\n", me, ps, middle);
    ADD("movement: fastads off\n");
    ADD("movement: after \"movement_patch fastads 0\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastjump slideangles], off [fastswap "
        "fastads]; t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n");
    ADD("movement: fast ADS: the perk test says no (thread %u, player state 0x%llx, sights going up, at 0.5, %s "
        "00000000 00000000 00000000 80000001 (specialty_fastads 0, specialty_fastreload 0), given by scripts 00000000 "
        "00000000 00000000 00000000; the game skips the speed-up\n", me, ps, middle);
    ADD("movement: fastads on, fast ADS multiplier 1.4\n");
    ADD("movement: after \"movement_patch fastads 1\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastads fastjump slideangles], off "
        "[fastswap]; t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n");
    ADD("movement: fast ADS: the doubling (0x100ed5e5c) reached for the first time on thread %u (player state "
        "0x%llx)\n", me, ps);
    ADD("movement: fast ADS: rate -0.00333333 a ms x multiplier 1.4 = -0.00466667 (the game alone: -0.00666667), "
        "sights at 0.5, weapon anim type 7 (sniper rifles 7 and 8)\n");
    ADD("movement: fastads off\n");
    ADD("movement: after \"movement_patch fastads 0\": on [slide_preserve_momentum jumpslowdown fast_sprintleap "
        "fast_sprintlerpin slideslowdown canslidethensprint instant_slide fastjump slideangles], off [fastswap "
        "fastads]; t7o_omnimovement 1, t7o_strafe_speed_scale 0.9 (strafe scale 0.9), fast ADS multiplier 1.4\n");
    ADD("movement: fast ADS: rate -0.00333333 a ms x 2 = -0.00666667 (the game's own), sights at 0.5, weapon anim "
        "type 7 (sniper rifles 7 and 8)\n");
    ADD("movement: fast ADS: the perk test (0x100ed5e2b) reached for the first time on thread %s (player state "
        "0x%llx)\n", elsewhere_thread, ps);
    ADD("movement: fast ADS: the perk test says no (thread %s, player state 0x%llx, sights going up, at 0.5, %s "
        "00000000 00000000 00000000 80000001 (specialty_fastads 0, specialty_fastreload 0), given by scripts 00000000 "
        "00000000 00000000 00000000; the game skips the speed-up\n", elsewhere_thread, ps, middle);
#undef ADD
    CHECK(strcmp(out, expected) == 0);
    if (strcmp(out, expected) != 0)
        printf("  printed:\n%s  expected:\n%s", out, expected);

    // no more than the most lines in all, the first-time lines aside
    reset();
    mod_loader();
    debug = true;
    printing_begin();
    for (uint32_t i = 0; i < 100; i++) {
        set_perks(i, 0);
        handler_FastAds_PerkTest(perk_first(line_ps, 0), perk_second(line_weapon, true));
    }
    debug = false;
    int lines = 0;
    for (const char *p = printing_end(); *p != '\0'; p++)
        lines += *p == '\n';
    CHECK(lines == Fastads_Lines_Max + 1);
    // the same for the doubling's lines (another rate each time)
    reset();
    mod_loader();
    debug = true;
    printing_begin();
    for (int i = 1; i <= 100; i++) {
        detour_registers_t registers;
        memset(&registers, 0, sizeof(registers));
        registers.rbx = (uint64_t)(uintptr_t)line_ps;
        registers.r12 = (uint64_t)(uintptr_t)line_weapon;
        float rate = 1.0f / (float)i;
        memcpy(registers.xmm[0], &rate, sizeof(rate));
        handler_FastAds_Double(&registers);
    }
    debug = false;
    lines = 0;
    for (const char *p = printing_end(); *p != '\0'; p++)
        lines += *p == '\n';
    CHECK(lines == Fastads_Lines_Max + 1);
    // the sniper anim types where the game keeps them
    CHECK(sniper_anim_types == game_sniper_anim_types);
}

// BO3MACFIX_MOVEFASTADSPERK: read at init, only with the setting on, and said
static void test_fastads_perk_switch(void) {
    reset();
    setenv("BO3MACFIX_MOVEFASTADSPERK", "1", 1);
    printing_begin();
    aae_movement_init(0x100000000, 1);
    const char *out = printing_end();
    CHECK(fastads_perk_forced && strcmp(out, "movement: BO3MACFIX_MOVEFASTADSPERK=1: while fastads is on, the player "
                                             "counts as having the fast-ADS perk at the game's test for it (for test "
                                             "runs: no console command gives a perk)\n") == 0);
    aae_movement_init(0x100000000, 0);
    CHECK(!fastads_perk_forced);
    setenv("BO3MACFIX_MOVEFASTADSPERK", "0", 1);
    aae_movement_init(0x100000000, 1);
    CHECK(!fastads_perk_forced);
    unsetenv("BO3MACFIX_MOVEFASTADSPERK");
    aae_movement_init(0x100000000, 1);
    CHECK(!fastads_perk_forced);
}

#if defined(__x86_64__)
// The perk test's thunk puts its two values together in assembly (Perk_Test_Setup): run that on chosen registers
// (rbx, r12, r13, r15 in setup_in) and keep what it leaves in rdi and rsi.
uint64_t setup_in[4], setup_out[2];
void run_perk_setup(void);
__asm__(".globl _run_perk_setup\n_run_perk_setup:\n push %rbx\n push %r12\n push %r13\n push %r15\n"
        " mov _setup_in(%rip), %rbx\n mov _setup_in+8(%rip), %r12\n mov _setup_in+16(%rip), %r13\n"
        " mov _setup_in+24(%rip), %r15\n" Perk_Test_Setup
        " mov %rdi, _setup_out(%rip)\n mov %rsi, _setup_out+8(%rip)\n"
        " pop %r15\n pop %r13\n pop %r12\n pop %rbx\n ret\n");
static void test_perk_setup(void) {
    static const uint64_t r15s[] = { 0, 1, 0x80, 0xff, 0xdeadbe00, 0xdeadbe01, 0x100 };
    static const uint64_t r13s[] = { 0, 1, 0x100, 0x101, 0xff00, 0xffff };
    for (size_t i = 0; i < sizeof(r15s) / sizeof(r15s[0]); i++) {
        for (size_t j = 0; j < sizeof(r13s) / sizeof(r13s[0]); j++) {
            setup_in[0] = 0x7ff7bfef1230ull, setup_in[1] = 0x00006000a0b0c0d0ull, setup_in[2] = r13s[j];
            setup_in[3] = r15s[i];
            run_perk_setup();
            uint8_t fast = (uint8_t)r15s[i];
            bool up = (uint8_t)r13s[j] != 0;
            CHECK(setup_out[0] == perk_first((void *)(uintptr_t)setup_in[0], fast));
            CHECK(setup_out[1] == perk_second((void *)(uintptr_t)setup_in[1], up));
        }
    }
    // and the handler takes them apart again
    reset();
    mod_loader();
    uint8_t ps[0xc00] = { 0 }, weapon[0x80] = { 0 };
    setup_in[0] = (uint64_t)(uintptr_t)ps, setup_in[1] = (uint64_t)(uintptr_t)weapon, setup_in[2] = 1;
    setup_in[3] = 0x4500;
    run_perk_setup();
    CHECK(!handler_FastAds_PerkTest(setup_out[0], setup_out[1]));
    setup_in[3] = 0x4501;
    run_perk_setup();
    CHECK(handler_FastAds_PerkTest(setup_out[0], setup_out[1]));
}
#endif

int main(void) {
    test_first_state();
    test_stoi();
    test_names();
    test_commands();
    test_strafe_rule();
    test_init();
    test_before_start();
    test_start();
    test_mod_loader();
    test_multiplier();
    test_live_dvars();
    test_mod_gate();
    test_forced();
    test_words();
    test_printed();
    test_threads();
    test_fastads_rule();
    test_fastads_detours();
    test_fastads_lines();
    test_fastads_perk_switch();
#if defined(__x86_64__)
    test_perk_setup();
#endif
    if (failures != 0) {
        printf("test_aae_movement: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_movement: all passed\n");
    return 0;
}
