// aae_breath.c: T7Overcharged's aim status, its names and when it is handed over, its two dvars, and the hint's hook
// against a made-up game.
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_plugin.c"
#include "../source/aae_breath.c"

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static struct {
    void *address, *replacement, **original;
} hooked[4];
static int hooks;
// the game's own hint function, made up: every call with what it was given, and when (the order of things)
static int order;
static struct {
    int calls, at;
    int local_client;
    void *element, *context, *lua;
    float floats[4];
} game_hint;
static void game_render(int local_client, void *element, void *context, float first, float second, float third,
                        float fourth, void *lua) {
    game_hint.calls++;
    game_hint.at = ++order;
    game_hint.local_client = local_client, game_hint.element = element, game_hint.context = context;
    game_hint.lua = lua;
    game_hint.floats[0] = first, game_hint.floats[1] = second, game_hint.floats[2] = third, game_hint.floats[3] = fourth;
}
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 4)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    if (out_origin_func == (void **)&orig_UIElement_RenderHoldBreathHint)
        *out_origin_func = (void *)game_render;
    hooks++;
    return 0;
}

// ---- the rule, against a made-up game that says what it is asked, in which order

static struct {
    aae_breath_weapon_t weapon;
    bool overlay, zoom_levels;
    char asked[8]; // w the weapon, o the overlay, z the zoom levels
    int count;
} rule_game;
static void ask(char what) {
    if (rule_game.count < 7) {
        rule_game.asked[rule_game.count++] = what;
        rule_game.asked[rule_game.count] = '\0';
    }
}
static void rule_weapon(void *context, aae_breath_weapon_t *weapon) {
    CHECK(context == &rule_game);
    ask('w');
    *weapon = rule_game.weapon;
}
static bool rule_overlay(void *context) {
    CHECK(context == &rule_game);
    ask('o');
    return rule_game.overlay;
}
static bool rule_zoom_levels(void *context) {
    CHECK(context == &rule_game);
    ask('z');
    return rule_game.zoom_levels;
}
static const aae_breath_game_t asked_game = { &rule_game, rule_weapon, rule_overlay, rule_zoom_levels };

// a player aiming all the way with nothing of the hold breath going on: the timers as the game keeps them then
static aae_breath_player_t aiming(void) {
    return (aae_breath_player_t){ .sights = 1.0f, .hold_time = -1, .breath_in_time = 0, .delay = 0,
                                  .frame_time = 16 };
}
// a sniper rifle: steadied by holding the breath, a scope overlay and zoom levels that differ
static void set_weapon(int weapon_class, bool steadies, bool airburst, bool overlay, bool zoom_levels) {
    rule_game.weapon = (aae_breath_weapon_t){ weapon_class, steadies, airburst };
    rule_game.overlay = overlay, rule_game.zoom_levels = zoom_levels;
}
// the status, and what was asked on the way, as "wo..."
static int status_asking(const aae_breath_player_t *player, const char *expected_questions) {
    rule_game.count = 0, rule_game.asked[0] = '\0';
    int status = aae_breath_status(player, &asked_game);
    if (strcmp(rule_game.asked, expected_questions) != 0) {
        printf("  asked \"%s\" instead of \"%s\"\n", rule_game.asked, expected_questions);
        CHECK(!"the game was asked something else");
    }
    return status;
}

static void test_weapon_rule(void) {
    aae_breath_player_t player = aiming();
    // a sniper rifle aimed all the way: holding the breath steadies it, the hint is up
    set_weapon(0, true, false, true, true);
    CHECK(status_asking(&player, "woz") == Aae_Breath_Can_Breath);
    // a weapon that isn't steadied: aiming, or aiming through a scope whose zoom levels differ
    set_weapon(0, false, false, true, true);
    CHECK(status_asking(&player, "woz") == Aae_Breath_Holding_Zoomed);
    set_weapon(0, false, false, true, false);
    CHECK(status_asking(&player, "woz") == Aae_Breath_Ads_On);
    // no overlay: the zoom levels aren't asked about
    set_weapon(0, false, false, false, true);
    CHECK(status_asking(&player, "wo") == Aae_Breath_Ads_On);
    set_weapon(0, true, false, false, true);
    CHECK(status_asking(&player, "wo") == Aae_Breath_Can_Breath);
    // one that bursts in the air isn't steadied, whatever its definition says
    set_weapon(0, true, true, true, true);
    CHECK(status_asking(&player, "woz") == Aae_Breath_Holding_Zoomed);
    set_weapon(0, true, true, false, false);
    CHECK(status_asking(&player, "wo") == Aae_Breath_Ads_On);
    // an item: nothing to say, and nothing more asked
    set_weapon(Weapon_Class_Item, true, false, true, true);
    CHECK(status_asking(&player, "w") == Aae_Breath_Ads_Off);
    set_weapon(Weapon_Class_Item - 1, true, false, true, true);
    CHECK(status_asking(&player, "woz") == Aae_Breath_Can_Breath);
    set_weapon(Weapon_Class_Item + 1, true, false, true, true);
    CHECK(status_asking(&player, "woz") == Aae_Breath_Can_Breath);
    // the sights not all the way up, by however little, or not a number: the weapon is looked at and that is all
    set_weapon(0, true, false, true, true);
    const float not_up[] = { 0.0f, 0.5f, 0.99999994f, 1.0000001f, 2.0f, -1.0f, NAN, INFINITY };
    for (size_t i = 0; i < sizeof(not_up) / sizeof(not_up[0]); i++) {
        player.sights = not_up[i];
        CHECK(status_asking(&player, "w") == Aae_Breath_Ads_Off);
    }
    player.sights = 1.0f;
    // eFlags 0x4000, or the breath held: the weapon isn't even looked at
    player.eflag_0x4000 = true;
    CHECK(status_asking(&player, "") == Aae_Breath_Ads_Off);
    player.eflag_0x4000 = false;
    player.holding = true;
    player.hold_time = 0;
    CHECK(status_asking(&player, "") == Aae_Breath_Holding);
}

// the timers' part, with the weapon's part giving `before`
static int timers(int before, bool holding, int32_t time, int32_t breath_in, int32_t delay, int32_t frame) {
    aae_breath_player_t player = { .holding = holding, .sights = 1.0f, .hold_time = time, .breath_in_time = breath_in,
                                   .delay = delay, .frame_time = frame };
    switch (before) {
    case Aae_Breath_Ads_Off:
        set_weapon(Weapon_Class_Item, false, false, false, false);
        break;
    case Aae_Breath_Ads_On:
        set_weapon(0, false, false, false, false);
        break;
    case Aae_Breath_Can_Breath:
        set_weapon(0, true, false, false, false);
        break;
    default:
        set_weapon(0, false, false, true, true);
    }
    // (while the breath is held the weapon's part is 0 anyway)
    rule_game.count = 0;
    return aae_breath_status(&player, &asked_game);
}

static void test_timer_rule(void) {
    // held: 3 while the breath is taken in, 9 after
    CHECK(timers(0, true, 0, 1000, 0, 16) == Aae_Breath_Holding);
    CHECK(timers(0, true, 1000, 1000, 1000, 16) == Aae_Breath_Holding);
    CHECK(timers(0, true, 1001, 1000, 0, 16) == Aae_Breath_Held);
    CHECK(timers(0, true, 0, 0, 0, 16) == Aae_Breath_Holding);
    CHECK(timers(0, true, 1, 0, 0, 16) == Aae_Breath_Held);
    CHECK(timers(0, true, 5000, 1000, 500, 16) == Aae_Breath_Held);
    // held from this frame on: 6 when no breath sound is waiting (the delay as it will be after this frame)
    CHECK(timers(0, true, -1, 0, 0, 16) == Aae_Breath_Starts);
    CHECK(timers(0, true, -1, 0, 16, 16) == Aae_Breath_Starts);
    CHECK(timers(0, true, -1, 0, 17, 16) == Aae_Breath_Ads_Off);
    CHECK(timers(0, true, -1, 0, -40, 16) == Aae_Breath_Starts);
    CHECK(timers(0, true, -1, 0, 1, 0) == Aae_Breath_Ads_Off);
    // a delay that is already 0 or below isn't counted down, whatever the frame says
    CHECK(timers(0, true, -1, 0, 0, -16) == Aae_Breath_Starts);
    CHECK(timers(0, false, 100, 0, 0, -16) == Aae_Breath_Breathes_Out);
    CHECK(timers(0, true, INT_MIN, 0, 0, 16) == Aae_Breath_Starts);
    // let go: a gasp after more than 4.5 seconds, counting this frame
    CHECK(timers(0, false, 4485, 1000, 900, 16) == Aae_Breath_Gasps);
    CHECK(timers(0, false, 4484, 1000, 900, 16) == Aae_Breath_Ads_Off);
    CHECK(timers(Aae_Breath_Can_Breath, false, 9000, 1000, 900, 16) == Aae_Breath_Gasps);
    // else breathing out, when its sound isn't held back by the delay
    CHECK(timers(0, false, 2000, 1000, 16, 16) == Aae_Breath_Breathes_Out);
    CHECK(timers(0, false, 2000, 1000, 0, 16) == Aae_Breath_Breathes_Out);
    CHECK(timers(0, false, 0, 1000, -5, 16) == Aae_Breath_Breathes_Out);
    CHECK(timers(Aae_Breath_Can_Breath, false, 2000, 1000, 17, 16) == Aae_Breath_Can_Breath);
    CHECK(timers(Aae_Breath_Ads_On, false, 4484, 1000, 1000, 16) == Aae_Breath_Ads_On);
    // nothing of it going on: the weapon's part stays
    CHECK(timers(Aae_Breath_Ads_Off, false, -1, 0, 0, 16) == Aae_Breath_Ads_Off);
    CHECK(timers(Aae_Breath_Ads_On, false, -1, 0, 0, 16) == Aae_Breath_Ads_On);
    CHECK(timers(Aae_Breath_Can_Breath, false, -1, 0, 0, 16) == Aae_Breath_Can_Breath);
    CHECK(timers(Aae_Breath_Holding_Zoomed, false, -1, 0, 0, 16) == Aae_Breath_Holding_Zoomed);
    CHECK(timers(Aae_Breath_Holding_Zoomed, false, INT_MIN, 0, 0, 16) == Aae_Breath_Holding_Zoomed);
    // the sums wrap at 32 bits, as the game's: a time near the top comes out below 0, a delay far down above it
    CHECK(timers(0, false, INT_MAX, 0, 0, 16) == Aae_Breath_Breathes_Out);
    CHECK(timers(0, false, INT_MAX, 0, 5, 1) == Aae_Breath_Ads_Off);
    CHECK(timers(0, true, -1, 0, 1, INT_MIN) == Aae_Breath_Starts);
    CHECK(timers(0, true, -1, 0, 1, INT_MIN + 2) == Aae_Breath_Ads_Off);
    CHECK(timers(0, false, 4500, 0, 0, INT_MIN) == Aae_Breath_Breathes_Out);
}

static void test_names(void) {
    CHECK(strcmp(aae_breath_status_name(0), "ADS_OFF") == 0 && strcmp(aae_breath_status_name(1), "ADS_ON.") == 0 &&
          strcmp(aae_breath_status_name(2), "CAN_BREATH") == 0 && strcmp(aae_breath_status_name(3), "HOLDING_BREATH") == 0 &&
          strcmp(aae_breath_status_name(4), "HOLDING_BREATH_ZOOMED") == 0);
    const int unknown[] = { 5, 6, 7, 8, 9, 10, -1, INT_MIN, INT_MAX };
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++)
        CHECK(strcmp(aae_breath_status_name(unknown[i]), "Unknown") == 0);
    CHECK(Aae_Breath_Ads_Off == 0 && Aae_Breath_Ads_On == 1 && Aae_Breath_Can_Breath == 2 && Aae_Breath_Holding == 3 &&
          Aae_Breath_Holding_Zoomed == 4 && Aae_Breath_Starts == 6 && Aae_Breath_Breathes_Out == 7 &&
          Aae_Breath_Gasps == 8 && Aae_Breath_Held == 9);
}

static void test_publish(void) {
    int last = -1;
    CHECK(aae_breath_publish(&last, 0) && last == 0);
    CHECK(!aae_breath_publish(&last, 0) && last == 0);
    CHECK(aae_breath_publish(&last, 3) && last == 3 && !aae_breath_publish(&last, 3));
    CHECK(aae_breath_publish(&last, 9) && aae_breath_publish(&last, 3) && last == 3);
    last = 7;
    CHECK(aae_breath_publish(&last, -1) && last == -1 && !aae_breath_publish(&last, -1));
}

// ---- the made-up game: two local clients' cg_t, a weapon, UI models and dvars

static uint8_t *game_cgs;
static int32_t game_client_count = 2;
static uint8_t *cg_of(int which) {
    return game_cgs + (size_t)which * Cg_Size;
}
static void set_int(uint8_t *at, int32_t value) {
    memcpy(at, &value, sizeof(value));
}
static void set_float(uint8_t *at, float value) {
    memcpy(at, &value, sizeof(value));
}
static void set_player(int which, bool holding, float sights, int32_t time, int32_t breath_in, int32_t delay) {
    uint8_t *cg = cg_of(which), *ps = cg + Cg_PlayerState;
    uint64_t flags = 0x1230 | (holding ? Weapon_Flag_Holding_Breath : 0);
    memcpy(ps + Ps_WeaponFlags, &flags, sizeof(flags));
    set_float(ps + Ps_Sights, sights);
    set_int(cg + Cg_HoldBreathTime, time);
    set_int(cg + Cg_HoldBreathInTime, breath_in);
    set_int(cg + Cg_HoldBreathDelay, delay);
    set_int(cg + Cg_FrameTime, 16);
}

// one held weapon per client (its number is the client's + 7), its definition, and the two questions
static uint8_t game_definition[2][0x1000];
static struct {
    int viewmodel_calls, definition_calls, overlay_calls, zoom_calls;
    const uint8_t *player_state;
    bool overlay[2], zoom_levels[2];
} game_weapon;
static uint64_t game_viewmodel_weapon(const uint8_t *player_state) {
    game_weapon.viewmodel_calls++;
    game_weapon.player_state = player_state;
    return player_state == cg_of(1) + Cg_PlayerState ? 8 : 7;
}
static const uint8_t *game_weapon_def(uint64_t weapon) {
    game_weapon.definition_calls++;
    CHECK(weapon == 7 || weapon == 8);
    return game_definition[weapon - 7];
}
static int game_overlay(uint64_t weapon) {
    game_weapon.overlay_calls++;
    return game_weapon.overlay[weapon - 7] ? 3 : 0;
}
static int game_zoom_levels(uint64_t weapon) {
    game_weapon.zoom_calls++;
    return game_weapon.zoom_levels[weapon - 7];
}
static void set_definition(int which, int32_t weapon_class, bool steadies, bool airburst) {
    set_int(game_definition[which] + WeaponDef_Class, weapon_class);
    game_definition[which][WeaponDef_Steadies] = steadies ? 0x5a : 0;
    game_definition[which][WeaponDef_Airburst] = airburst ? 0x5a : 0;
}

static int game_controller[2] = { 0, 1 };
static int game_controller_index(int local_client) {
    return local_client >= 0 && local_client < 2 ? game_controller[local_client] : -1;
}
// UI models: a root per controller (100, 101) and children made by name as they are asked for
static struct {
    uint16_t parent;
    char name[32];
    int32_t number;
    char text[32];
    int sets, notified, kind; // 0 none yet, 1 a number, 2 text
} game_models[16];
static int game_model_count, model_writes;
static uint16_t game_model_for_controller(int controller) {
    CHECK(controller >= 0 && controller < Controller_Models);
    return (uint16_t)(100 + controller);
}
static uint16_t game_model_child(uint16_t model, const char *name) {
    for (int i = 0; i < game_model_count; i++)
        if (game_models[i].parent == model && strcmp(game_models[i].name, name) == 0)
            return (uint16_t)(i + 1);
    if (game_model_count == 16)
        return 0;
    game_models[game_model_count].parent = model;
    snprintf(game_models[game_model_count].name, sizeof(game_models[0].name), "%s", name);
    return (uint16_t)++game_model_count;
}
static bool game_model_set_int(uint16_t model, int value) {
    order++;
    CHECK(model >= 1 && model <= game_model_count);
    game_models[model - 1].number = value, game_models[model - 1].kind = 1, game_models[model - 1].sets++;
    model_writes++;
    return true;
}
static bool game_model_set_string(uint16_t model, const char *value) {
    order++;
    CHECK(model >= 1 && model <= game_model_count);
    snprintf(game_models[model - 1].text, sizeof(game_models[0].text), "%s", value);
    game_models[model - 1].kind = 2, game_models[model - 1].sets++;
    model_writes++;
    return true;
}
static bool game_model_notify(uint16_t model) {
    order++;
    CHECK(model >= 1 && model <= game_model_count);
    game_models[model - 1].notified++;
    model_writes++;
    return true;
}
// the model at root/hudItems/name, or NULL when there is none
static typeof(game_models[0]) *model_at(int controller, const char *name) {
    for (int i = 0; i < game_model_count; i++)
        if (game_models[i].parent == 100 + controller && strcmp(game_models[i].name, "hudItems") == 0)
            for (int j = 0; j < game_model_count; j++)
                if (game_models[j].parent == i + 1 && strcmp(game_models[j].name, name) == 0)
                    return &game_models[j];
    return NULL;
}

// dvars: an int one keeps its value when it is set to one out of its range, as the game's Dvar_SetVariant does
static struct {
    uint8_t game_side[0x80]; // the hash at +0, the name at +8
    bool there, is_int;
    int min, max, value, flags;
    char text[32], description[32];
} game_dvars[4];
static int registrations, dvar_sets;
static uint8_t *game_register_int(uint32_t hash, const char *name, int value, int min, int max, uint32_t flags,
                                  const char *description) {
    registrations++;
    CHECK(hash == gsc_hash(name));
    for (int i = 0; i < 4; i++) {
        if (!game_dvars[i].there) {
            typeof(game_dvars[0]) *dvar = &game_dvars[i];
            memset(dvar, 0, sizeof(*dvar));
            memcpy(dvar->game_side, &hash, sizeof(hash));
            dvar->there = true, dvar->is_int = true, dvar->min = min, dvar->max = max, dvar->value = value;
            dvar->flags = (int)flags;
            snprintf(dvar->description, sizeof(dvar->description), "%s", description);
            return dvar->game_side;
        }
    }
    return NULL;
}
static uint8_t *game_register_none(uint32_t hash, const char *name, int value, int min, int max, uint32_t flags,
                                   const char *description) {
    (void)hash, (void)name, (void)value, (void)min, (void)max, (void)flags, (void)description;
    registrations++;
    return NULL;
}
static const char *dvar_name(int i) {
    const char *name;
    memcpy(&name, game_dvars[i].game_side + Dvar_Name, sizeof(name));
    return name;
}
static typeof(game_dvars[0]) *dvar_named(const char *name) {
    for (int i = 0; i < 4; i++) {
        uint32_t hash;
        memcpy(&hash, game_dvars[i].game_side, sizeof(hash));
        if (game_dvars[i].there && hash == gsc_hash(name))
            return &game_dvars[i];
    }
    return NULL;
}
static char last_set_text[32]; // what the dvar was last set to, as text
static void game_set_by_name(const char *name, const char *value, bool create) {
    order++;
    dvar_sets++;
    snprintf(last_set_text, sizeof(last_set_text), "%s", value);
    CHECK(create);
    typeof(game_dvars[0]) *dvar = dvar_named(name);
    if (dvar == NULL) {
        CHECK(!"a dvar set that wasn't registered");
        return;
    }
    int number = atoi(value);
    if (dvar->is_int && number >= dvar->min && number <= dvar->max)
        dvar->value = number;
    snprintf(dvar->text, sizeof(dvar->text), "%s", value);
}
static const uint8_t *game_find_dvar(uint32_t hash) {
    for (int i = 0; i < 4; i++) {
        uint32_t own;
        memcpy(&own, game_dvars[i].game_side, sizeof(own));
        if (game_dvars[i].there && own == hash)
            return game_dvars[i].game_side;
    }
    return NULL;
}
static int game_dvar_int(const uint8_t *dvar) {
    for (int i = 0; i < 4; i++)
        if (game_dvars[i].game_side == dvar)
            return game_dvars[i].value;
    CHECK(!"no such dvar");
    return 0;
}

// ---- the mod with the plugin, and time for aae_plugin_loaded_lately
static char loaded_name[Mod_Name_Size + 8], plugin_file[4096 + 64];
static uint64_t plugin_time = 5000;
static uint64_t test_plugin_now(void) {
    return plugin_time;
}
static void set_mod(bool loaded) {
    memset(loaded_name, 0, sizeof(loaded_name));
    if (loaded)
        snprintf(loaded_name, sizeof(loaded_name), "%s", strrchr(mod_folder, '/') + 1);
    plugin_time += Plugin_Look_Ms;
}

// the renderer calls the hint's function for a local client: what the game's own was given, and that it came first
static void render(int local_client) {
    static int element, context, lua;
    int calls = game_hint.calls, before = order;
    fake_UIElement_RenderHoldBreathHint(local_client, &element, &context, 0.25f, -3.5f, 1e9f, 0.75f, &lua);
    CHECK(game_hint.calls == calls + 1 && game_hint.local_client == local_client && game_hint.element == &element &&
          game_hint.context == &context && game_hint.lua == &lua && game_hint.floats[0] == 0.25f &&
          game_hint.floats[1] == -3.5f && game_hint.floats[2] == 1e9f && game_hint.floats[3] == 0.75f);
    // nothing was handed over before the game's function ran (the game draws, then the status is worked out)
    CHECK(game_hint.at == before + 1);
}
// what the mod sees: the dvar, the two models of the first controller
static bool mod_sees(int dvar_value, int model_number, const char *model_text) {
    typeof(game_dvars[0]) *aim = dvar_named("aim_status");
    typeof(game_models[0]) *number = model_at(0, "breathStatus"), *text = model_at(0, "breathStatusText");
    char text_of[16];
    snprintf(text_of, sizeof(text_of), "%d", model_number);
    return aim != NULL && aim->value == dvar_value && strcmp(last_set_text, text_of) == 0 && number != NULL && number->kind == 1 &&
           number->number == model_number && text != NULL && text->kind == 2 && strcmp(text->text, model_text) == 0;
}

static void reset_game(void) {
    memset(game_dvars, 0, sizeof(game_dvars));
    memset(game_models, 0, sizeof(game_models));
    game_model_count = model_writes = registrations = dvar_sets = 0;
    memset(&game_weapon, 0, sizeof(game_weapon));
    memset(&game_hint, 0, sizeof(game_hint));
    atomic_store(&started, false);
    last_status = -1;
    game_client_count = 2;
    game_controller[0] = 0, game_controller[1] = 1;
}

static void test_dvars(void) {
    reset_game();
    // switched off: nothing is registered, and the hint is the game's alone
    enabled = 0;
    aae_breath_t7o_started();
    CHECK(registrations == 0 && !atomic_load(&started));
    enabled = 1;
    aae_breath_t7o_started();
    // T7Overcharged's order, its values and descriptions, no flags, and the names written into the dvars
    CHECK(registrations == 2 && atomic_load(&started));
    CHECK(game_dvars[0].there && strcmp(dvar_name(0), "holdBreathTimer") == 0 && game_dvars[0].value == 0 &&
          game_dvars[0].min == 0 && game_dvars[0].max == 5000 && game_dvars[0].flags == 0 &&
          strcmp(game_dvars[0].description, "") == 0);
    CHECK(game_dvars[1].there && strcmp(dvar_name(1), "aim_status") == 0 && game_dvars[1].value == 0 &&
          game_dvars[1].min == 0 && game_dvars[1].max == 5 && game_dvars[1].flags == 0 &&
          strcmp(game_dvars[1].description, "aim type") == 0);
    // once: the mod starts T7Overcharged again at every map
    aae_breath_t7o_started();
    CHECK(registrations == 2);
    // the debug line reads the dvars back as the game holds them
    reset_game();
    debug = 1;
    aae_breath_t7o_started();
    debug = 0;
    CHECK(registrations == 2 && atomic_load(&started));
    // a game that gives no dvar back: nothing is written into one, and T7Overcharged counts as started
    reset_game();
    Dvar_RegisterInt = game_register_none;
    debug = 1;
    aae_breath_t7o_started();
    debug = 0;
    CHECK(registrations == 2 && atomic_load(&started));
    Dvar_RegisterInt = game_register_int;
}

static void test_hook(void) {
    reset_game();
    Dvar_RegisterInt = game_register_int;
    set_mod(true);
    set_definition(0, 0, true, false); // a sniper rifle for the first client
    set_definition(1, 0, false, false);
    game_weapon.overlay[0] = game_weapon.zoom_levels[0] = true;
    set_player(0, false, 0.0f, -1, 0, 0);
    set_player(1, false, 0.0f, -1, 0, 0);
    // before the mod has started T7Overcharged: the game's hint only
    render(0);
    CHECK(model_writes == 0 && dvar_sets == 0 && game_weapon.viewmodel_calls == 0);
    aae_breath_t7o_started();
    // not aiming
    render(0);
    CHECK(mod_sees(0, 0, "ADS_OFF") && dvar_sets == 1 && model_at(0, "breathStatus")->notified == 0);
    CHECK(game_weapon.viewmodel_calls == 1 && game_weapon.player_state == cg_of(0) + Cg_PlayerState &&
          game_weapon.definition_calls == 1 && game_weapon.overlay_calls == 0);
    // the models: one hudItems with two children, the number and the name; hudItems is notified
    int hud_items = -1;
    for (int i = 0; i < game_model_count; i++)
        if (game_models[i].parent == 100 && strcmp(game_models[i].name, "hudItems") == 0)
            hud_items = i;
    CHECK(hud_items >= 0 && game_models[hud_items].notified == 1 && game_model_count == 3);
    // the same again: nothing handed over
    int writes = model_writes;
    render(0);
    CHECK(model_writes == writes && dvar_sets == 1);
    // aiming all the way with the sniper rifle: the hint is up
    set_player(0, false, 1.0f, -1, 0, 0);
    render(0);
    CHECK(mod_sees(2, 2, "CAN_BREATH") && dvar_sets == 2 && game_models[hud_items].notified == 2);
    CHECK(game_weapon.overlay_calls == 1 && game_weapon.zoom_calls == 1);
    // the breath held from this frame on: 6 is out of aim_status's range, the game keeps 2 there
    set_player(0, true, 1.0f, -1, 0, 0);
    render(0);
    CHECK(mod_sees(2, 6, "Unknown") && dvar_sets == 3);
    CHECK(game_weapon.viewmodel_calls == 3); // not looked at while the breath is held
    // taken in, then held (the time against how long it is taken in for, not against the delay)
    set_player(0, true, 1.0f, 100, 1000, 900);
    render(0);
    CHECK(mod_sees(3, 3, "HOLDING_BREATH"));
    set_player(0, true, 1.0f, 950, 1000, 900);
    render(0);
    CHECK(mod_sees(3, 3, "HOLDING_BREATH") && dvar_sets == 4);
    set_player(0, true, 1.0f, 1200, 1000, 0);
    render(0);
    CHECK(mod_sees(3, 9, "Unknown"));
    // held too long: a gasp; then the game has its time at -1 again
    set_player(0, false, 1.0f, 4600, 1000, 0);
    render(0);
    CHECK(mod_sees(3, 8, "Unknown"));
    set_player(0, false, 1.0f, -1, 0, 900);
    render(0);
    CHECK(mod_sees(2, 2, "CAN_BREATH"));
    // let go early: the breath out
    set_player(0, false, 1.0f, 2000, 1000, 0);
    render(0);
    CHECK(mod_sees(2, 7, "Unknown"));
    // the frame's ms count: 4490 and this frame's 16 are more than 4.5 seconds; a delay of 10 is over this frame
    set_player(0, false, 1.0f, 4490, 1000, 0);
    render(0);
    CHECK(mod_sees(2, 8, "Unknown"));
    set_player(0, true, 1.0f, -1, 0, 10);
    render(0);
    CHECK(mod_sees(2, 6, "Unknown"));
    // a weapon that bursts in the air isn't steadied
    set_definition(0, 0, true, true);
    set_player(0, false, 1.0f, -1, 0, 0);
    render(0);
    CHECK(mod_sees(4, 4, "HOLDING_BREATH_ZOOMED"));
    // a weapon that isn't steadied, through a scope with zoom levels, and without
    set_definition(0, 0, false, false);
    set_player(0, false, 1.0f, -1, 0, 0);
    render(0);
    CHECK(mod_sees(4, 4, "HOLDING_BREATH_ZOOMED"));
    game_weapon.zoom_levels[0] = false;
    render(0);
    CHECK(mod_sees(1, 1, "ADS_ON."));
    // eFlags 0x4000
    set_int(cg_of(0) + Cg_PlayerState + Ps_EFlags, EFlag_0x4000 | 0x10);
    int looked = game_weapon.viewmodel_calls;
    render(0);
    CHECK(mod_sees(0, 0, "ADS_OFF") && game_weapon.viewmodel_calls == looked);
    set_int(cg_of(0) + Cg_PlayerState + Ps_EFlags, ~EFlag_0x4000);
    render(0);
    CHECK(mod_sees(1, 1, "ADS_ON."));
    // the second local client has its own state and its own controller's models; the last status is one for both
    set_player(1, false, 1.0f, -1, 0, 0);
    int sets = dvar_sets;
    render(1);
    CHECK(dvar_sets == sets && game_weapon.player_state == cg_of(1) + Cg_PlayerState); // 1, as the first's was
    set_definition(1, 0, true, false);
    render(1);
    CHECK(dvar_sets == sets + 1 && dvar_named("aim_status")->value == 2 && model_at(1, "breathStatus") != NULL &&
          model_at(1, "breathStatus")->number == 2 && model_at(0, "breathStatus")->number == 1);
    // a client without a controller of the table's two: the dvar only
    game_controller[1] = 2;
    set_definition(1, 0, false, false);
    writes = model_writes;
    render(1);
    CHECK(dvar_sets == sets + 2 && dvar_named("aim_status")->value == 1 && model_writes == writes);
    game_controller[1] = -1;
    set_definition(1, Weapon_Class_Item, false, false);
    render(1);
    CHECK(dvar_sets == sets + 3 && dvar_named("aim_status")->value == 0 && model_writes == writes);
    game_controller[1] = 1;

    // not a local client of the game's, or no cg_t at all: nothing
    sets = dvar_sets, writes = model_writes, looked = game_weapon.viewmodel_calls;
    game_client_count = 1;
    render(1);
    render(2);
    render(-1);
    game_client_count = 2;
    uint8_t *all = game_cgs;
    game_cgs = NULL;
    render(0);
    game_cgs = all;
    CHECK(dvar_sets == sets && model_writes == writes && game_weapon.viewmodel_calls == looked);

    // without the mod: nothing, and the last status stays what it was
    set_definition(0, 0, true, false);
    set_mod(false);
    render(0);
    CHECK(dvar_sets == sets && model_writes == writes && game_weapon.viewmodel_calls == looked);
    set_mod(true);
    render(0);
    CHECK(mod_sees(2, 2, "CAN_BREATH") && dvar_sets == sets + 1);

    // the debug lines read what they print without changing anything
    debug = 1;
    set_player(0, true, 1.0f, 100, 1000, 900);
    render(0);
    set_player(0, false, 1.0f, -1, 0, 0);
    render(0);
    set_definition(0, Weapon_Class_Item, false, false);
    render(0);
    debug = 0;
    CHECK(mod_sees(0, 0, "ADS_OFF"));
}

static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 4; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    // nothing handed over yet: the first status, whatever it is, goes to the mod
    CHECK(last_status == -1);
    hooks = 0;
    aae_breath_init(base, 0);
    CHECK(hooks == 0 && enabled == 0);
    aae_breath_init(base, 1);
    CHECK(hooks == 1 && enabled == 1 &&
          was_hooked(ADDR_UIElement_RenderHoldBreathHint, (void *)fake_UIElement_RenderHoldBreathHint,
                     (void **)&orig_UIElement_RenderHoldBreathHint));
    CHECK(cg_array == (void *)(base + ADDR_cgArray) && local_client_count == (void *)(base + ADDR_LOCAL_CLIENT_COUNT) &&
          BG_GetViewmodelWeapon == (void *)(base + ADDR_BG_GetViewmodelWeapon) &&
          BG_GetWeaponDef == (void *)(base + ADDR_BG_GetWeaponDef) &&
          BG_WeaponOverlayReticle == (void *)(base + ADDR_BG_WeaponOverlayReticle) &&
          BG_WeaponHasZoomLevels == (void *)(base + ADDR_BG_WeaponHasZoomLevels) &&
          Com_LocalClient_GetControllerIndex == (void *)(base + ADDR_Com_LocalClient_GetControllerIndex) &&
          UIModel_ForController == (void *)(base + ADDR_UIModel_ForController) &&
          UIModel_Child == (void *)(base + ADDR_UIModel_Child) && UIModel_SetInt == (void *)(base + ADDR_UIModel_SetInt) &&
          UIModel_SetString == (void *)(base + ADDR_UIModel_SetString) &&
          UIModel_ForceNotify == (void *)(base + ADDR_UIModel_ForceNotify) &&
          Dvar_RegisterInt == (void *)(base + ADDR_Dvar_RegisterInt) &&
          Dvar_SetFromStringByName == (void *)(base + ADDR_Dvar_SetFromStringByName) &&
          Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) && Dvar_GetInt == (void *)(base + ADDR_Dvar_GetInt) &&
          loaded_mod == (const char *)(base + ADDR_Mods_LoadedName));
    // the places, as they were read from the game's code
    CHECK(ADDR_UIElement_RenderHoldBreathHint == 0x1130467 && ADDR_BG_GetViewmodelWeapon == 0xed3bf3 &&
          ADDR_BG_GetWeaponDef == 0x12362cd && ADDR_BG_WeaponOverlayReticle == 0x138606c &&
          ADDR_BG_WeaponHasZoomLevels == 0x138337e && ADDR_Com_LocalClient_GetControllerIndex == 0xeae63e &&
          ADDR_UIModel_ForController == 0x1036404 && ADDR_UIModel_Child == 0x10365c3 &&
          ADDR_UIModel_SetInt == 0x1036905 && ADDR_UIModel_SetString == 0x103698e &&
          ADDR_UIModel_ForceNotify == 0x1036af8 && ADDR_Dvar_RegisterInt == 0xfa149e &&
          ADDR_Dvar_SetFromStringByName == 0xfa3386);
    CHECK(Cg_Size == 0x342a80 && Cg_FrameTime == 0x11a8a8 && Cg_PlayerState == 0x11a8d0 && Ps_WeaponFlags == 0x18 &&
          Ps_EFlags == 0x15c && Ps_Sights == 0x2fc && Ps_HoldBreathScale == 0x778 && Ps_HoldBreathTimer == 0x77c &&
          Cg_HoldBreathTime == 0x2e77d0 && Cg_HoldBreathInTime == 0x2e77d4 && Cg_HoldBreathDelay == 0x2e77d8 &&
          Weapon_Flag_Holding_Breath == 4 && EFlag_0x4000 == 0x4000 && WeaponDef_Class == 0x70 &&
          WeaponDef_Steadies == 0xdc8 && WeaponDef_Airburst == 0xe67 && Dvar_Name == 8 && Controller_Models == 2 &&
          Weapon_Class_Item == 10 && Gasp_After_Ms == 4500);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-breath-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/2631943123", template);
    char command[4096 + 64];
    snprintf(command, sizeof(command), "mkdir -p '%s/T7Overcharged/plugins'", mod_folder);
    if (system(command) != 0)
        return 2;
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    plugin_now_ms = test_plugin_now;
    game_cgs = calloc(3, Cg_Size);

    test_init(); // first: it points the code at the game, and the rest at the made-up one
    loaded_mod = loaded_name;
    cg_array = &game_cgs;
    local_client_count = &game_client_count;
    BG_GetViewmodelWeapon = game_viewmodel_weapon;
    BG_GetWeaponDef = game_weapon_def;
    BG_WeaponOverlayReticle = game_overlay;
    BG_WeaponHasZoomLevels = game_zoom_levels;
    Com_LocalClient_GetControllerIndex = game_controller_index;
    UIModel_ForController = game_model_for_controller;
    UIModel_Child = game_model_child;
    UIModel_SetInt = game_model_set_int;
    UIModel_SetString = game_model_set_string;
    UIModel_ForceNotify = game_model_notify;
    Dvar_RegisterInt = game_register_int;
    Dvar_SetFromStringByName = game_set_by_name;
    Dvar_FindByHash = game_find_dvar;
    Dvar_GetInt = game_dvar_int;
    test_weapon_rule();
    test_timer_rule();
    test_names();
    test_publish();
    test_dvars();
    test_hook();

    free(game_cgs);
    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_breath: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_breath: all passed\n");
    return 0;
}
