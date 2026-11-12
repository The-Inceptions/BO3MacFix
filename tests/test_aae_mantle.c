// aae_mantle.c: the mod's rules for traversal, and the hooks against a made-up game: its dvars, its scan with the
// traces it does, and the per-mode dvar that turns traversal on (the test includes the source itself).
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include "../source/aae_mantle.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}
bool prepatch_has_hook(uint32_t offset) {
    (void)offset;
    return true;
}
static char mod_folder[1024];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static char loaded_name[Mod_Name_Size + 8]; // the game's array is Mod_Name_Size long; what follows isn't its

// ---- the made-up game

// the mod's dvars: a value each, or missing
static struct {
    bool there;
    float value;
} options[Opt_Count];
static uint8_t trm_enabled[0x80], enabled_zombies[0x80], enabled_multiplayer[0x80], enabled_campaign[0x80];
static bool have_trm_enabled;

static void *game_find_dvar(uint32_t hash) {
    for (int i = 0; i < Opt_Count; i++)
        if (hash == gsc_hash(option_names[i]))
            return options[i].there ? &options[i] : NULL;
    if (hash == gsc_hash("trm_enabled"))
        return have_trm_enabled ? trm_enabled : NULL;
    return NULL;
}
static float game_dvar_float(void *dvar) {
    float value;
    memcpy(&value, (uint8_t *)dvar + offsetof(__typeof__(options[0]), value), sizeof(value));
    return value;
}
static void set_option(int which, float value) {
    options[which].there = true;
    options[which].value = value;
}

static float game_setting(const char *name, bool which) {
    (void)which;
    return name != NULL && strcmp(name, "trm_maxHeight") == 0 ? 0.0f : 7.0f;
}

// the scan: its first trace hits what the test says, a second one hits a mantle brush, and the result is what the
// game's own rule makes of the first: a ledge unless that was a mantle brush
static uint8_t player_state[0x900], pmove[0x100];
static struct {
    int calls, traces;
    signed char forward, right;      // the move input the scan saw
    uint32_t contents, surface;      // what the first trace hits
    float fraction;
    bool start_solid;
    uint32_t seen_first, seen_second; // the contents the scan read back
} game_scan;

static void game_trace(uint8_t *pm, uint8_t *results, const float *start, const float *mins, const float *maxs,
                       const float *end, int pass_entity, int contents_mask) {
    (void)pm, (void)start, (void)mins, (void)maxs, (void)end, (void)pass_entity, (void)contents_mask;
    memset(results, 0, 0x60);
    bool first = game_scan.traces++ == 0;
    float fraction = first ? game_scan.fraction : 0.5f;
    uint32_t contents = first ? game_scan.contents : Contents_Mantle, surface = first ? game_scan.surface : 0;
    memcpy(results + Trace_Fraction, &fraction, sizeof(fraction));
    memcpy(results + Trace_Contents, &contents, sizeof(contents));
    memcpy(results + Trace_SurfaceFlags, &surface, sizeof(surface));
    results[Trace_StartSolid] = first && game_scan.start_solid;
}

static bool game_scan_ledge(uint8_t *pm, void *pml, vec3_t *ledge, vec3_t *normal) {
    (void)pml, (void)normal;
    uint8_t results[0x60];
    float zero[3] = { 0 };
    game_scan.calls++;
    game_scan.traces = 0;
    game_scan.forward = (signed char)pm[Pm_Forwardmove];
    game_scan.right = (signed char)pm[Pm_Forwardmove + 1];
    fake_PM_PlayerTrace(pm, results, zero, zero, zero, zero, 0, 0x1010011);
    memcpy(&game_scan.seen_first, results + Trace_Contents, sizeof(uint32_t));
    float fraction;
    memcpy(&fraction, results + Trace_Fraction, sizeof(fraction));
    bool go_on = !results[Trace_StartSolid] && fraction != 1.0f && (game_scan.seen_first & Contents_Mantle) == 0;
    fake_PM_PlayerTrace(pm, results, zero, zero, zero, zero, 0, 0x1010011);
    memcpy(&game_scan.seen_second, results + Trace_Contents, sizeof(uint32_t));
    ledge->z = 140.0f;
    return go_on;
}

static bool game_can_start(uint8_t *pm, void *pml) {
    (void)pml;
    return abs((signed char)pm[Pm_Forwardmove]) + abs((signed char)pm[Pm_Forwardmove + 1]) > Game_Push;
}

static struct {
    int calls;
    float ledge_z;
} game_type;
static int game_trm_type(uint8_t *ps, vec3_t ledge) {
    (void)ps;
    game_type.calls++;
    game_type.ledge_z = ledge.z;
    return 7;
}

static int inits;
static bool zombies_at_init;
static void game_trm_init(void *alloc) {
    (void)alloc;
    inits++;
    zombies_at_init = enabled_zombies[Dvar_Value] != 0;
}

static void set_input(int forward, int right) {
    pmove[Pm_Forwardmove] = (uint8_t)(signed char)forward;
    pmove[Pm_Forwardmove + 1] = (uint8_t)(signed char)right;
}
static uint32_t traversal_flags(void) {
    uint32_t flags;
    memcpy(&flags, player_state + Ps_TraversalFlags, sizeof(flags));
    return flags;
}
static bool scan_now(uint32_t contents, uint32_t surface) {
    vec3_t ledge = { 0 }, normal = { 0 };
    memset(player_state + Ps_TraversalFlags, 0, 4);
    game_scan.contents = contents, game_scan.surface = surface, game_scan.fraction = 0.4f, game_scan.start_solid = false;
    return fake_TRM_Scan(pmove, NULL, &ledge, &normal);
}

static void per_mode_dvar(uint8_t *zombies, uint8_t *multiplayer, uint8_t *campaign) {
    int per_mode = Dvar_Type_PerMode, a_bool = Dvar_Type_Bool;
    memset(trm_enabled, 0, sizeof(trm_enabled));
    memset(enabled_zombies, 0, sizeof(enabled_zombies));
    memset(enabled_multiplayer, 0, sizeof(enabled_multiplayer));
    memset(enabled_campaign, 0, sizeof(enabled_campaign));
    memcpy(trm_enabled + Dvar_Type, &per_mode, sizeof(per_mode));
    memcpy(enabled_zombies + Dvar_Type, &a_bool, sizeof(a_bool));
    memcpy(enabled_multiplayer + Dvar_Type, &a_bool, sizeof(a_bool));
    memcpy(enabled_campaign + Dvar_Type, &a_bool, sizeof(a_bool));
    enabled_multiplayer[Dvar_Value] = enabled_multiplayer[Dvar_Latched] = enabled_multiplayer[Dvar_Default] = 1;
    enabled_campaign[Dvar_Value] = enabled_campaign[Dvar_Latched] = enabled_campaign[Dvar_Default] = 1;
    uint8_t *modes[3] = { zombies, multiplayer, campaign };
    memcpy(trm_enabled + Dvar_Modes, modes, sizeof(modes));
    have_trm_enabled = true;
}
static bool zombies_on(void) {
    return enabled_zombies[Dvar_Value] == 1 && enabled_zombies[Dvar_Latched] == 1 && enabled_zombies[Dvar_Default] == 1;
}
static bool zombies_off(void) {
    return enabled_zombies[Dvar_Value] == 0 && enabled_zombies[Dvar_Latched] == 0 && enabled_zombies[Dvar_Default] == 0;
}

// ---- the rules by themselves

static void test_input_rules(void) {
    static const int thresholds[] = { -1, 0, 1, 20, 49, 50, 51, 60, 100, 126, 127, 200, 253, 254, 256 };
    for (size_t t = 0; t < sizeof(thresholds) / sizeof(thresholds[0]); t++) {
        int threshold = thresholds[t];
        for (int f = -127; f <= 127; f++) {
            for (int r = -127; r <= 127; r++) {
                int size = abs(f) + abs(r);
                // the scan: pushing is any input beyond the threshold
                signed char sf = (signed char)f, sr = (signed char)r;
                aae_mantle_scan_input(threshold, &sf, &sr);
                bool pushing = size != 0 && size > threshold;
                if ((abs(sf) + abs(sr) > Game_Push) != pushing) {
                    printf("FAIL scan input: threshold %d, %d %d became %d %d\n", threshold, f, r, sf, sr);
                    failures++;
                    return;
                }
                // pushing keeps its direction exactly: both grow by the same whole factor
                if (pushing && (sf * r != sr * f || (f != 0 && sf / f < 1) || (r != 0 && sr / r < 1))) {
                    printf("FAIL scan direction: threshold %d, %d %d became %d %d\n", threshold, f, r, sf, sr);
                    failures++;
                    return;
                }
                // the start: no input is needed for a threshold of 0 or less
                signed char cf = (signed char)f, cr = (signed char)r;
                aae_mantle_start_input(threshold, &cf, &cr);
                bool allowed = threshold <= 0 || size > threshold;
                if ((abs(cf) + abs(cr) > Game_Push) != allowed) {
                    printf("FAIL start input: threshold %d, %d %d became %d %d\n", threshold, f, r, cf, cr);
                    failures++;
                    return;
                }
            }
        }
    }
    // what already agrees with the game's own test isn't touched
    signed char f = 30, r = -40;
    aae_mantle_scan_input(50, &f, &r);
    CHECK(f == 30 && r == -40);
    f = 10, r = -5;
    aae_mantle_scan_input(0, &f, &r);
    CHECK(f == 40 && r == -20);
    f = 100, r = 20;
    aae_mantle_scan_input(125, &f, &r);
    CHECK(abs(f) + abs(r) <= Game_Push && f > 0 && r > 0);
}

static void test_first_hit(void) {
    uint32_t flags = 0;
    // the mod's rule: a ledge is looked for at a mantle brush, the opposite of the game's own
    CHECK((aae_mantle_first_hit(false, Contents_Mantle | 1, 0, &flags) & Contents_Mantle) == 0);
    CHECK((aae_mantle_first_hit(false, 1, 0, &flags) & Contents_Mantle) != 0);
    CHECK((aae_mantle_first_hit(false, 0x430600, 0xd440a0, &flags) & Contents_Mantle) != 0);
    // without the checks: anywhere
    CHECK((aae_mantle_first_hit(true, Contents_Mantle | 1, 0, &flags) & Contents_Mantle) == 0);
    CHECK((aae_mantle_first_hit(true, 1, 0, &flags) & Contents_Mantle) == 0);
    // the other contents stay as they are
    CHECK(aae_mantle_first_hit(false, Contents_Mantle | 0x10001, 0, &flags) == 0x10001);
    CHECK(aae_mantle_first_hit(false, 0x10001, 0, &flags) == (Contents_Mantle | 0x10001));
    CHECK(flags == 0);
    // "mantle over" and "mount" together, without "mantle on": the traversal goes over
    aae_mantle_first_hit(false, Contents_Mantle, 0x18000000, &flags);
    CHECK(flags == Traversal_Over);
    static const uint32_t not_over[] = { 0x08000000, 0x10000000, 0x20000000, 0x38000000, 0x28000000, 0x30000000, 0 };
    for (size_t i = 0; i < sizeof(not_over) / sizeof(not_over[0]); i++) {
        flags = 0x40;
        aae_mantle_first_hit(true, 1, not_over[i], &flags);
        CHECK(flags == 0x40);
    }
    flags = 0x40;
    aae_mantle_first_hit(true, 1, 0x18000000 | 0x440a0, &flags);
    CHECK(flags == (0x40 | Traversal_Over));
}

static void test_types(void) {
    // below the first step: the game's own choice
    CHECK(aae_mantle_high_type(95.9f, 0.0f, 0) == -1 && aae_mantle_high_type(15.0f, 0.0f, Traversal_Over) == -1);
    // from 96: onto the ledge, the high one from 350
    CHECK(aae_mantle_high_type(96.0f, 0.0f, 0) == 0x14 && aae_mantle_high_type(349.9f, 0.0f, 0) == 0x14);
    CHECK(aae_mantle_high_type(350.0f, 0.0f, 0) == 0x15 && aae_mantle_high_type(4000.0f, 0.0f, 0) == 0x15);
    // over it: three heights
    CHECK(aae_mantle_high_type(96.0f, 0.0f, Traversal_Over) == 0x11);
    CHECK(aae_mantle_high_type(199.9f, 0.0f, Traversal_Over) == 0x11);
    CHECK(aae_mantle_high_type(200.0f, 0.0f, Traversal_Over) == 0x12);
    CHECK(aae_mantle_high_type(350.0f, 0.0f, Traversal_Over) == 0x13);
    // other flags don't matter
    CHECK(aae_mantle_high_type(120.0f, 0.0f, 0x3e) == 0x14 && aae_mantle_high_type(120.0f, 0.0f, 0x3f) == 0x11);
    // the mod's offset moves all three steps
    CHECK(aae_mantle_high_type(100.0f, 10.0f, 0) == -1 && aae_mantle_high_type(106.0f, 10.0f, 0) == 0x14);
    CHECK(aae_mantle_high_type(355.0f, 10.0f, 0) == 0x14 && aae_mantle_high_type(360.0f, 10.0f, 0) == 0x15);
    CHECK(aae_mantle_high_type(60.0f, -45.0f, 0) == 0x14 && aae_mantle_high_type(50.9f, -45.0f, 0) == -1);
    CHECK(aae_mantle_high_type(160.0f, -45.0f, Traversal_Over) == 0x12);
    // what the game's own choice is shown: never a height of its high kinds
    CHECK(aae_mantle_low_height(40.0f) == 40.0f && aae_mantle_low_height(95.5f) == 95.5f);
    CHECK(aae_mantle_low_height(96.0f) < 96.0f && aae_mantle_low_height(96.0f) > 95.0f);
    CHECK(aae_mantle_low_height(105.9f) < 96.0f && aae_mantle_low_height(105.9f) > 95.0f);
}

// ---- the hooks

static void test_without_the_mod(void) {
    // nothing of the mod's is set: everything is the game's own
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 0.0f);
    CHECK(fake_GameTypeSettings_GetFloat("other", false) == 7.0f && fake_GameTypeSettings_GetFloat(NULL, false) == 7.0f);
    set_input(20, 0);
    CHECK(scan_now(1, 0x18000000) && game_scan.forward == 20 && game_scan.seen_first == 1 && traversal_flags() == 0);
    CHECK(!scan_now(Contents_Mantle, 0) && game_scan.seen_first == Contents_Mantle);
    CHECK(!fake_TRM_CanStart(pmove, NULL));
    set_input(60, 0);
    CHECK(fake_TRM_CanStart(pmove, NULL));
    float feet = 100.0f;
    memcpy(player_state + Ps_Origin_Z, &feet, sizeof(feet));
    game_type.calls = 0;
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 300.0f }) == 7 && game_type.calls == 1 &&
          game_type.ledge_z == 300.0f);
}

// the mod's dvars are still there from earlier in the session, but no mod with the plugin is loaded
static void test_after_the_mod(void) {
    atomic_store(&active, false);
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 0.0f);
    set_input(20, 0);
    CHECK(scan_now(1, 0x18000000) && game_scan.forward == 20 && game_scan.seen_first == 1 && traversal_flags() == 0);
    CHECK(!scan_now(Contents_Mantle, 0) && game_scan.seen_first == Contents_Mantle);
    set_input(0, 0);
    CHECK(!fake_TRM_CanStart(pmove, NULL));
    game_type.calls = 0;
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 300.0f }) == 7 && game_type.calls == 1 &&
          game_type.ledge_z == 300.0f);
}

static void test_with_the_mod(void) {
    atomic_store(&active, true);
    set_option(Opt_Height, 60.0f);
    // the height: the mod's, and the plugin's 57 for one out of its range
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 60.0f);
    CHECK(fake_GameTypeSettings_GetFloat("other", false) == 7.0f && fake_GameTypeSettings_GetFloat(NULL, false) == 7.0f);
    set_option(Opt_Height, 5000.0f);
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 57.0f);
    set_option(Opt_Height, -1.0f);
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 57.0f);
    set_option(Opt_Height, 0.0f);
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 0.0f);
    set_option(Opt_Height, 35.0f);

    // the scan with the other three still missing: checks on, any input pushes
    set_input(0, 0);
    CHECK(!scan_now(1, 0) && game_scan.seen_first == (Contents_Mantle | 1) && game_scan.forward == 0);
    CHECK(scan_now(Contents_Mantle | 1, 0) && game_scan.seen_first == 1);
    // only the first trace of a scan is the mod's business
    CHECK(game_scan.seen_second == Contents_Mantle && game_scan.traces == 2);
    // and the move input is the player's own again afterwards
    set_input(3, -4);
    CHECK(scan_now(Contents_Mantle, 0) && game_scan.forward == 24 && game_scan.right == -32);
    CHECK((signed char)pmove[Pm_Forwardmove] == 3 && (signed char)pmove[Pm_Forwardmove + 1] == -4);
    // a mantle brush for going over
    CHECK(scan_now(Contents_Mantle, 0x18000000) && traversal_flags() == Traversal_Over);
    CHECK(scan_now(Contents_Mantle, 0x08000000) && traversal_flags() == 0);

    // a first trace that hit nothing, or started inside something, is left alone, and doesn't leave the next trace
    // of that scan marked as a first one
    game_scan.contents = 1, game_scan.surface = 0x18000000, game_scan.fraction = 1.0f, game_scan.start_solid = false;
    vec3_t ledge = { 0 }, normal = { 0 };
    memset(player_state + Ps_TraversalFlags, 0, 4);
    CHECK(!fake_TRM_Scan(pmove, NULL, &ledge, &normal) && game_scan.seen_first == 1 && traversal_flags() == 0);
    CHECK(game_scan.seen_second == Contents_Mantle);
    game_scan.fraction = 0.4f, game_scan.start_solid = true;
    CHECK(!fake_TRM_Scan(pmove, NULL, &ledge, &normal) && game_scan.seen_first == 1 && traversal_flags() == 0);
    // a trace outside a scan is nobody's
    uint8_t results[0x60];
    float zero[3] = { 0 };
    game_scan.traces = 0, game_scan.start_solid = false;
    fake_PM_PlayerTrace(pmove, results, zero, zero, zero, zero, 0, 0);
    uint32_t contents;
    memcpy(&contents, results + Trace_Contents, sizeof(contents));
    CHECK(contents == 1 && traversal_flags() == 0);

    // without the checks: anywhere
    set_option(Opt_IgnoreChecks, 1.0f);
    CHECK(scan_now(1, 0) && game_scan.seen_first == 1);
    CHECK(scan_now(Contents_Mantle | 1, 0) && game_scan.seen_first == 1);
    set_option(Opt_IgnoreChecks, 0.0f);
    CHECK(!scan_now(1, 0));

    // the threshold: for the scan, and for the start
    set_option(Opt_Threshold, 60.0f);
    set_input(55, 0);
    scan_now(Contents_Mantle, 0);
    CHECK(abs(game_scan.forward) <= Game_Push && !fake_TRM_CanStart(pmove, NULL));
    set_input(61, 0);
    scan_now(Contents_Mantle, 0);
    CHECK(game_scan.forward == 61 && fake_TRM_CanStart(pmove, NULL));
    set_option(Opt_Threshold, 0.0f);
    set_input(0, 0);
    CHECK(fake_TRM_CanStart(pmove, NULL) && pmove[Pm_Forwardmove] == 0 && pmove[Pm_Forwardmove + 1] == 0);
    set_option(Opt_Threshold, -5.0f); // the plugin's lowest is -1
    CHECK(fake_TRM_CanStart(pmove, NULL));
    set_option(Opt_Threshold, 300.0f); // beyond its highest: 0
    CHECK(fake_TRM_CanStart(pmove, NULL));
    set_option(Opt_Threshold, 0.0f);

    // the kind of traversal
    float feet = 100.0f;
    memcpy(player_state + Ps_Origin_Z, &feet, sizeof(feet));
    memset(player_state + Ps_TraversalFlags, 0, 4);
    game_type.calls = 0;
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 160.0f }) == 7 && game_type.calls == 1 &&
          game_type.ledge_z == 160.0f);
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 200.0f }) == 0x14 && game_type.calls == 1);
    uint32_t over = Traversal_Over;
    memcpy(player_state + Ps_TraversalFlags, &over, sizeof(over));
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 300.0f }) == 0x12 && game_type.calls == 1);
    // with the mod's offset a ledge between the game's first step and the mod's is the game's choice, which is
    // shown a height below its own step
    set_option(Opt_HighOffset, 10.0f);
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 200.0f }) == 7 && game_type.calls == 2 &&
          game_type.ledge_z < 196.0f && game_type.ledge_z > 195.0f);
    set_option(Opt_HighOffset, 1000.0f); // the plugin's highest is 512: the first step is 608 above the feet then
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 707.0f }) == 7 && game_type.calls == 3);
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 708.0f }) == 0x11 && game_type.calls == 3);
    set_option(Opt_HighOffset, -100.0f); // and its lowest -45: 51
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 150.0f }) == 7 && game_type.calls == 4);
    CHECK(fake_TRM_Type(player_state, (vec3_t){ 0, 0, 151.0f }) == 0x11 && game_type.calls == 4);
    set_option(Opt_HighOffset, 0.0f);
}

static void make_plugin(const char *folder, bool there) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/T7Overcharged", folder);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/T7Overcharged/plugins", folder);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/%s", folder, Plugin_Path);
    if (there) {
        int file = open(path, O_CREAT | O_WRONLY, 0644);
        CHECK(file >= 0);
        close(file);
    } else {
        unlink(path);
    }
}
static void remove_mod(const char *folder) {
    char path[1200];
    make_plugin(folder, false);
    snprintf(path, sizeof(path), "%s/T7Overcharged/plugins", folder);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/T7Overcharged", folder);
    rmdir(path);
    rmdir(folder);
}
// the game loads the mod in this folder (NULL: none); T7Overcharged was last asked for from asked_from
static void load_mod(const char *folder, const char *asked_from) {
    memset(loaded_name, '#', sizeof(loaded_name)); // the game's array has no room for more than the name
    memset(loaded_name, 0, Mod_Name_Size);
    if (folder != NULL) {
        const char *name = strrchr(folder, '/') + 1;
        memcpy(loaded_name, name, strlen(name) < Mod_Name_Size ? strlen(name) : Mod_Name_Size);
    }
    snprintf(mod_folder, sizeof(mod_folder), "%s", asked_from != NULL ? asked_from : "");
}
static void start_map(void) {
    int before = inits;
    fake_TRM_Init(NULL);
    CHECK(inits == before + 1);
}
static void fresh_dvar(void) {
    per_mode_dvar(enabled_zombies, enabled_multiplayer, enabled_campaign);
    zombies_own.changed = false;
}

static void test_traversal_for_zombies(void) {
    char base[] = "/tmp/bo3macfix-test-mantle-XXXXXX", mod[1100], other[1100], long_mod[1100], longer_mod[1100];
    CHECK(mkdtemp(base) != NULL);
    snprintf(mod, sizeof(mod), "%s/2631943123", base);
    snprintf(other, sizeof(other), "%s/another_mod", base);
    snprintf(long_mod, sizeof(long_mod), "%s/a_mod_whose_name_is_32_long_____", base);    // fills the game's array
    snprintf(longer_mod, sizeof(longer_mod), "%s/a_mod_whose_name_is_32_long_____x", base); // and one that doesn't fit
    CHECK(strlen(strrchr(long_mod, '/') + 1) == Mod_Name_Size);
    CHECK(mkdir(mod, 0755) == 0 && mkdir(other, 0755) == 0 && mkdir(long_mod, 0755) == 0 && mkdir(longer_mod, 0755) == 0);
    make_plugin(mod, true);
    make_plugin(other, false);
    make_plugin(long_mod, true);
    make_plugin(longer_mod, true);

    // no mod, or nothing has asked for T7Overcharged: zombies stays without traversal
    fresh_dvar();
    load_mod(NULL, NULL);
    start_map();
    CHECK(zombies_off() && !zombies_at_init && !atomic_load(&active));
    load_mod(mod, NULL);
    start_map();
    CHECK(zombies_off() && !atomic_load(&active));
    // a mod without the plugin
    load_mod(other, other);
    start_map();
    CHECK(zombies_off() && !atomic_load(&active));
    // the plugin's mod was loaded earlier in the session, another one is now, or none
    load_mod(other, mod);
    start_map();
    CHECK(zombies_off() && !atomic_load(&active));
    load_mod(NULL, mod);
    start_map();
    CHECK(zombies_off() && !atomic_load(&active));

    // the mod with the plugin: on, before the game makes the animations, and the rules count
    load_mod(mod, mod);
    start_map();
    CHECK(zombies_on() && zombies_at_init && atomic_load(&active));
    CHECK(enabled_multiplayer[Dvar_Value] == 1 && enabled_campaign[Dvar_Value] == 1);
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 35.0f);
    start_map(); // the next map, and the client's call after the server's
    CHECK(zombies_on() && atomic_load(&active));
    // the mod is unloaded: zombies as the game had it, and the game's own rules, though the mod's dvars are there
    load_mod(NULL, mod);
    start_map();
    CHECK(zombies_off() && !zombies_at_init && !atomic_load(&active));
    CHECK(fake_GameTypeSettings_GetFloat("trm_maxHeight", false) == 0.0f);
    start_map();
    CHECK(zombies_off());
    // and loaded again
    load_mod(mod, mod);
    start_map();
    CHECK(zombies_on() && atomic_load(&active));

    // what zombies had is what comes back, whatever it was
    static const uint8_t own[][3] = { { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 }, { 1, 1, 1 } };
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        fresh_dvar();
        enabled_zombies[Dvar_Value] = own[i][0], enabled_zombies[Dvar_Latched] = own[i][1];
        enabled_zombies[Dvar_Default] = own[i][2];
        // without the plugin's mod it isn't touched at all
        load_mod(NULL, NULL);
        start_map();
        CHECK(enabled_zombies[Dvar_Value] == own[i][0] && enabled_zombies[Dvar_Latched] == own[i][1] &&
              enabled_zombies[Dvar_Default] == own[i][2] && !zombies_own.changed);
        load_mod(mod, mod);
        start_map();
        CHECK(zombies_on());
        load_mod(other, other);
        start_map();
        CHECK(enabled_zombies[Dvar_Value] == own[i][0] && enabled_zombies[Dvar_Latched] == own[i][1] &&
              enabled_zombies[Dvar_Default] == own[i][2]);
    }
    // and what it has by the time the mod is loaded again, not what it had the time before
    fresh_dvar();
    load_mod(mod, mod);
    start_map();
    load_mod(NULL, mod);
    start_map();
    CHECK(zombies_off());
    enabled_zombies[Dvar_Latched] = 1;
    load_mod(mod, mod);
    start_map();
    CHECK(zombies_on());
    load_mod(NULL, mod);
    start_map();
    CHECK(enabled_zombies[Dvar_Value] == 0 && enabled_zombies[Dvar_Latched] == 1 && enabled_zombies[Dvar_Default] == 0);

    // no mod loaded, and a folder whose last part is as empty as the loaded mod's name
    char with_slash[1200];
    snprintf(with_slash, sizeof(with_slash), "%s/", mod);
    fresh_dvar();
    load_mod(NULL, with_slash);
    start_map();
    CHECK(zombies_off() && !atomic_load(&active));

    // a name as long as the game's array (no terminator in it), and a longer folder that only starts the same
    fresh_dvar();
    load_mod(long_mod, long_mod);
    start_map();
    CHECK(zombies_on());
    fresh_dvar();
    load_mod(long_mod, longer_mod);
    start_map();
    CHECK(zombies_off());
    fresh_dvar();
    load_mod(longer_mod, long_mod); // the game's array holds the first 32 characters of the longer name
    start_map();
    CHECK(zombies_on());

    // multiplayer's default is what counts, not what it's set to now
    fresh_dvar();
    load_mod(mod, mod);
    enabled_multiplayer[Dvar_Value] = 0;
    start_map();
    CHECK(zombies_on());
    fresh_dvar();
    enabled_multiplayer[Dvar_Default] = 0;
    enabled_zombies[Dvar_Value] = 1;
    start_map();
    CHECK(zombies_off());
    // without multiplayer's, the campaign's
    per_mode_dvar(enabled_zombies, NULL, enabled_campaign);
    zombies_own.changed = false;
    start_map();
    CHECK(zombies_on());
    // anything that isn't that kind of dvar is left alone
    per_mode_dvar(NULL, enabled_multiplayer, enabled_campaign);
    zombies_own.changed = false;
    start_map();
    per_mode_dvar(enabled_zombies, NULL, NULL);
    start_map();
    CHECK(zombies_off() && !zombies_own.changed);
    fresh_dvar();
    int a_bool = Dvar_Type_Bool, a_float = 2;
    memcpy(trm_enabled + Dvar_Type, &a_bool, sizeof(a_bool));
    start_map();
    CHECK(zombies_off());
    fresh_dvar();
    memcpy(enabled_zombies + Dvar_Type, &a_float, sizeof(a_float));
    start_map();
    CHECK(zombies_off());
    fresh_dvar();
    memcpy(enabled_multiplayer + Dvar_Type, &a_float, sizeof(a_float));
    start_map();
    CHECK(zombies_off());
    have_trm_enabled = false;
    start_map();
    CHECK(atomic_load(&active)); // the rules don't depend on that dvar

    remove_mod(mod);
    remove_mod(other);
    remove_mod(long_mod);
    remove_mod(longer_mod);
    rmdir(base);
}

int main(void) {
    Dvar_FindByHash = game_find_dvar;
    Dvar_GetFloat = game_dvar_float;
    orig_GameTypeSettings_GetFloat = game_setting;
    orig_PM_PlayerTrace = game_trace;
    orig_TRM_Scan = game_scan_ledge;
    orig_TRM_CanStart = game_can_start;
    orig_TRM_Type = game_trm_type;
    orig_TRM_Init = game_trm_init;
    loaded_mod = loaded_name;
    uint8_t *ps = player_state;
    memcpy(pmove, &ps, sizeof(ps));

    test_input_rules();
    test_first_hit();
    test_types();
    test_without_the_mod();
    test_with_the_mod();
    test_after_the_mod();
    test_traversal_for_zombies();
    if (failures != 0) {
        printf("test_aae_mantle: %d failures\n", failures);
        return 1;
    }
    printf("test_aae_mantle: all passed\n");
    return 0;
}
