// T7Overcharged's breath component, as All-around Enhancement 4's build of it has it (AAEHoldBreath).
//
// Holding your breath with a scope is the game's own, on the Mac as on Windows: aiming down a scope that sways, the
// sprint button holds the breath for up to 4.5 seconds (9.5 with the perk for it), the sway steadies, the player
// breathes out or gasps when it is let go, and the HUD element that the mod's HUD sets up with setupHoldBreathHint says
// "Hold [key] to steady" while that can be done. T7Overcharged leaves all of that as it is. Its breath component puts
// a function of its own in the place of the game's one that draws that hint, which draws the same hint (the game's
// tests for cg_drawBreathHint, the killcam, a flashbang, spectating, demo playback, aiming a Javelin, "alternate aim
// params" and one client state left out) and works out an "aim status" each time it is called. When the status is
// another than the last one, it hands it to the mod: the UI models hudItems.breathStatus (the number) and
// hudItems.breathStatusText (its name) of the client's controller, and its own dvar aim_status (the number as text).
// It also registers two int dvars when it starts: aim_status (0, 0 to 5, "aim type") and holdBreathTimer (0, 0 to
// 5000), which nothing reads.
//
// The status, by T7Overcharged's rules (aae_breath_status):
// - 0 while the breath is held or the player state's eFlags has 0x4000; and with a weapon that is an item, or whose
//   sights aren't all the way up;
// - else 1, or 4 when the weapon has a scope overlay and zoom levels that differ (the game's own tests before its
//   "Hold [key] to zoom" hint); then 2 when holding the breath steadies the weapon and it doesn't burst in the air
//   (where T7Overcharged draws its hint);
// - then the game's hold breath timers in the cg_t, read the way the game's next look at them will go: while the
//   breath is held, 3 while it is taken in and 9 after that, or 6 in the frame it starts with a sound; when it was
//   let go, 8 when it was held longer than 4.5 seconds, else 7 when the breath out sound is due; else as it was.
// The names: 0 "ADS_OFF", 1 "ADS_ON." (with the point), 2 "CAN_BREATH", 3 "HOLDING_BREATH", 4
// "HOLDING_BREATH_ZOOMED", 6 to 9 "Unknown". aim_status takes 0 to 5 only: the game keeps it as it was for 6 to 9,
// so a player holding their breath reads 3 there the whole time (Dvar_SetVariant refuses an int out of its range).
//
// What the mod does with it, in 4.1.7: nothing reads the two models; aim_status is read by one function of
// aae_core.csc (with the current weapon's attachments "+reddot", "+ir", "+dualoptic" and "+acog", against "3"), which
// only a function of the same script calls that nothing calls. So this changes nothing a player sees yet, on Windows
// either; it is here so that the mod's scripts and HUD find what T7Overcharged gives them.
//
// The Mac keeps the game's own hint: the hook below runs the game's function, then works out and hands over the status
// as T7Overcharged does. So the hint is still left out where the game leaves it out (see above); T7Overcharged draws
// it there too, with the render call's fourth float as its alpha where the game takes the element's own.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_breath.h"
#include "aae_plugin.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"

// the sums below compare a float as the game's code does; nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Weapon_Class_Item 10
#define Gasp_After_Ms 4500 // held longer than this, the game makes the player gasp

int aae_breath_status(const aae_breath_player_t *player, const aae_breath_game_t *game) {
    int status = Aae_Breath_Ads_Off;
    if (!player->holding && !player->eflag_0x4000) {
        aae_breath_weapon_t weapon;
        game->weapon(game->context, &weapon);
        // the sights all the way up is exactly 1 (a value that isn't a number isn't)
        if (weapon.weapon_class != Weapon_Class_Item && player->sights == 1.0f) {
            status = Aae_Breath_Ads_On;
            if (game->overlay(game->context) && game->zoom_levels(game->context))
                status = Aae_Breath_Holding_Zoomed;
            if (weapon.steadies && !weapon.airburst)
                status = Aae_Breath_Can_Breath;
        }
    }
    // the game's next look at its timers: the delay goes down by the frame's ms first, while there is one; the sums
    // wrap at 32 bits as the game's do
    int32_t delay = player->delay;
    if (delay > 0)
        delay = (int32_t)((uint32_t)delay - (uint32_t)player->frame_time);
    int32_t time = player->hold_time;
    if (player->holding) {
        if (time >= 0)
            status = time > player->breath_in_time ? Aae_Breath_Held : Aae_Breath_Holding;
        else if (delay <= 0)
            status = Aae_Breath_Starts;
    } else if (time >= 0) {
        time = (int32_t)((uint32_t)time + (uint32_t)player->frame_time);
        if (time > Gasp_After_Ms)
            status = Aae_Breath_Gasps;
        else if (delay <= 0)
            status = Aae_Breath_Breathes_Out;
    }
    return status;
}

const char *aae_breath_status_name(int status) {
    switch (status) {
    case Aae_Breath_Ads_Off:
        return "ADS_OFF";
    case Aae_Breath_Ads_On:
        return "ADS_ON.";
    case Aae_Breath_Can_Breath:
        return "CAN_BREATH";
    case Aae_Breath_Holding:
        return "HOLDING_BREATH";
    case Aae_Breath_Holding_Zoomed:
        return "HOLDING_BREATH_ZOOMED";
    default:
        return "Unknown";
    }
}

bool aae_breath_publish(int *last, int status) {
    if (status == *last)
        return false;
    *last = status;
    return true;
}

// ---- the game's side

#define Cg_Size 0x342a80
#define Cg_FrameTime 0x11a8a8         // int: the last frame's ms (Windows +0x11a888)
#define Cg_PlayerState 0x11a8d0       // the predicted player state (Windows +0x11a8b0)
#define Ps_WeaponFlags 0x18           // 64 bits; 4 while the breath is held
#define Ps_EFlags 0x15c
#define Ps_Sights 0x2fc               // fWeaponPosFrac
#define Ps_HoldBreathScale 0x778      // a float: how much of the sway is left (pmove), for the debug lines
#define Ps_HoldBreathTimer 0x77c      // an int: how long the breath has been held (pmove), for the debug lines
#define Cg_HoldBreathTime 0x2e77d0    // ints: the game's hold breath time, -1 while it isn't held (Windows +0x2e7670)
#define Cg_HoldBreathInTime 0x2e77d4
#define Cg_HoldBreathDelay 0x2e77d8
#define Weapon_Flag_Holding_Breath 0x4
#define EFlag_0x4000 0x4000
#define WeaponDef_Class 0x70          // weapClass, an int
#define WeaponDef_Steadies 0xdc8      // bHoldBreathToSteady
#define WeaponDef_Airburst 0xe67      // bAirburstWeapon
#define Dvar_Name 0x8                 // const char *: a dvar's name, which T7Overcharged writes after registering one
#define Dvar_Type 0x1c                // for the debug lines: its kind (6 an int), its value, and an int's range, which
#define Dvar_Value 0x28               // the game's Dvar_SetVariant (0x100fa0c1d) checks a new value against
#define Dvar_Domain 0x70
#define WeaponDef_OverlayName 0x0     // const char *: for the debug lines (szOverlayName in T7Overcharged's types)
#define Controller_Models 2           // the game's table of UI models has "controller0" and "controller1"

static int enabled, debug;
static _Atomic bool started; // T7Overcharged has been started by the mod and its dvars are there
static int last_status = -1; // the status last handed over; only the UI's thread comes here
static uint8_t *const *cg_array;
static const int32_t *local_client_count;
static uint64_t (*BG_GetViewmodelWeapon)(const uint8_t *player_state);
static const uint8_t *(*BG_GetWeaponDef)(uint64_t weapon);
static int (*BG_WeaponOverlayReticle)(uint64_t weapon);
static int (*BG_WeaponHasZoomLevels)(uint64_t weapon);
static int (*Com_LocalClient_GetControllerIndex)(int local_client);
static uint16_t (*UIModel_ForController)(int controller);
static uint16_t (*UIModel_Child)(uint16_t model, const char *name); // makes it when it isn't there
static bool (*UIModel_SetInt)(uint16_t model, int value);
static bool (*UIModel_SetString)(uint16_t model, const char *value);
static bool (*UIModel_ForceNotify)(uint16_t model);
static uint8_t *(*Dvar_RegisterInt)(uint32_t hash, const char *name, int value, int min, int max, uint32_t flags,
                                    const char *description);
static void (*Dvar_SetFromStringByName)(const char *name, const char *value, bool create);
static const uint8_t *(*Dvar_FindByHash)(uint32_t hash);
static int (*Dvar_GetInt)(const uint8_t *dvar);

static int32_t read_int(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}
static float read_float(const uint8_t *at) {
    float value;
    memcpy(&value, at, sizeof(value));
    return value;
}

// the held weapon and what was asked about it, for the questions and the debug lines
typedef struct {
    const uint8_t *player_state;
    uint64_t weapon;
    const uint8_t *definition;
    aae_breath_weapon_t facts;
    int asked; // how many of the three questions were asked
    bool overlay, zoom_levels;
} held_weapon_t;

static void held_weapon(void *context, aae_breath_weapon_t *weapon) {
    held_weapon_t *held = context;
    held->weapon = BG_GetViewmodelWeapon(held->player_state);
    const uint8_t *definition = BG_GetWeaponDef(held->weapon);
    held->definition = definition;
    held->facts.weapon_class = read_int(definition + WeaponDef_Class);
    held->facts.steadies = definition[WeaponDef_Steadies] != 0;
    held->facts.airburst = definition[WeaponDef_Airburst] != 0;
    held->asked = 1;
    *weapon = held->facts;
}
static bool held_overlay(void *context) {
    held_weapon_t *held = context;
    held->overlay = BG_WeaponOverlayReticle(held->weapon) != 0;
    held->asked = 2;
    return held->overlay;
}
static bool held_zoom_levels(void *context) {
    held_weapon_t *held = context;
    held->zoom_levels = BG_WeaponHasZoomLevels(held->weapon) != 0;
    held->asked = 3;
    return held->zoom_levels;
}

// The status goes to the client's controller's two models and to the dvar. T7Overcharged hands on whatever controller
// the game says, and the game's table of models has two: here a client without one of those gets the dvar only.
static void publish(int local_client, int status) {
    const char *name = aae_breath_status_name(status);
    int controller = Com_LocalClient_GetControllerIndex(local_client);
    if (controller >= 0 && controller < Controller_Models) {
        uint16_t hud_items = UIModel_Child(UIModel_ForController(controller), "hudItems");
        uint16_t number = UIModel_Child(hud_items, "breathStatus");
        uint16_t text = UIModel_Child(hud_items, "breathStatusText");
        UIModel_SetInt(number, status);
        UIModel_SetString(text, name);
        UIModel_ForceNotify(hud_items);
    }
    char value[16];
    snprintf(value, sizeof(value), "%d", status);
    Dvar_SetFromStringByName("aim_status", value, true);
}

static void debug_line(int local_client, int status, const aae_breath_player_t *player, const held_weapon_t *held) {
    const uint8_t *aim_status = Dvar_FindByHash(gsc_hash("aim_status"));
    printf("hold breath: local client %d, status %d %s (aim_status now %d): sights %.2f, holding %d, game's time %d, "
           "taken in for %d, delay %d, frame %d ms; breath held %d ms in pmove, sway scale %.2f", local_client, status,
           aae_breath_status_name(status), aim_status != NULL ? Dvar_GetInt(aim_status) : -1, (double)player->sights,
           player->holding, player->hold_time, player->breath_in_time, player->delay, player->frame_time,
           read_int(held->player_state + Ps_HoldBreathTimer),
           (double)read_float(held->player_state + Ps_HoldBreathScale));
    if (held->asked >= 1) {
        const char *overlay_name;
        memcpy(&overlay_name, held->definition + WeaponDef_OverlayName, sizeof(overlay_name));
        printf("; weapon class %d, steadies %d, airburst %d, overlay name \"%s\"", held->facts.weapon_class,
               held->facts.steadies, held->facts.airburst, overlay_name != NULL ? overlay_name : "");
    }
    if (held->asked >= 2)
        printf(", scope overlay %d", held->overlay);
    if (held->asked >= 3)
        printf(", zoom levels %d", held->zoom_levels);
    printf("\n");
}

// a local client's cg_t, NULL for anything that isn't one of the game's local clients
static const uint8_t *client_cg(int local_client) {
    uint8_t *clients = *cg_array;
    if (clients == NULL || local_client < 0 || local_client >= *local_client_count)
        return NULL;
    return clients + (size_t)local_client * Cg_Size;
}

// T7Overcharged's status for this local client, handed over when it changed. T7Overcharged takes any client number
// it is called with; the game only draws the hint for its local clients, which are what is let through here.
static void breath_status(int local_client) {
    const uint8_t *cg = client_cg(local_client);
    if (cg == NULL)
        return;
    const uint8_t *player_state = cg + Cg_PlayerState;
    aae_breath_player_t player = {
        .holding = (player_state[Ps_WeaponFlags] & Weapon_Flag_Holding_Breath) != 0,
        .eflag_0x4000 = (read_int(player_state + Ps_EFlags) & EFlag_0x4000) != 0,
        .sights = read_float(player_state + Ps_Sights),
        .hold_time = read_int(cg + Cg_HoldBreathTime),
        .breath_in_time = read_int(cg + Cg_HoldBreathInTime),
        .delay = read_int(cg + Cg_HoldBreathDelay),
        .frame_time = read_int(cg + Cg_FrameTime),
    };
    held_weapon_t held = { .player_state = player_state };
    aae_breath_game_t game = { &held, held_weapon, held_overlay, held_zoom_levels };
    int status = aae_breath_status(&player, &game);
    if (!aae_breath_publish(&last_status, status))
        return;
    publish(local_client, status);
    if (debug)
        debug_line(local_client, status, &player, &held);
}

// ---- the debug lines' view of the game itself: what it looks at before it holds the breath and before it draws its
// hint. Neither is changed by T7Overcharged or by this file; the lines say why a hold or a hint doesn't come.

#define Ps_QuickScopeTimer 0x790      // an int: ms left of the weapon's quick scope (counted down while aimed all the way)
#define Ps_Perks 0xb04                // perk n is bit n % 32 of the int n / 32 (BG_HasPerk, 0x1010a181d)
#define Perk_Deadshot 11              // specialty_deadshot (the game's perk names, 0x101b80610)
#define Perk_HoldBreath 38            // specialty_holdbreath: a breath lasts 9500 ms instead of 4500
#define Quick_Scope_Dvar_Hash 0xd86c86c // a bool dvar (default 1, registered at 0x101073ac4; its name isn't in the game)
#define Draw_Hint_Dvar_Hash 0xd900e011 // cg_drawBreathHint (default 1)
#define Key_Buttons_Size 0x498        // one local client's key buttons
#define Key_Button_Breath 0x1b0       // pressed by +holdbreath and by +breath_sprint (the left stick's by default)
#define Key_Button_Sprint 0x300       // pressed by +sprint and by +breath_sprint
#define Key_Button_Active 0x10        // a byte: the button is down
#define Hint_Visibility_Bits 0xa0a800000ULL // a killcam, flashbanged, spectating, a demo playing, a Javelin aimed
#define Local_Client_UI_Size 0x1078
#define Local_Client_UI_Hint 0x10     // the int the hint function wants 0
#define Cg_AlternateAim 0x333ef0      // a byte (Windows +0x333b90), set by UseAlternateAimParams
#define Element_Alpha 0x74            // the alpha of the element's own colour, which the game draws its hint with
#define Debug_Clients 2

static const uint8_t *key_buttons, *local_client_ui;
static const uint64_t *visibility_bits;
// where these lines go (the tests read them)
static int (*say)(const char *format, ...) = printf;

// What the game's pmove looks at before it holds the breath (Mac 0x100ed8d05.., Windows 0x14265e9e5..). The command's
// button bit 18 it asks for comes from the breath key button.
typedef struct {
    float sights;
    int32_t weapon_class, quick_scope_ms, breath_timer_ms;
    bool steadies, breath_button, quick_scope_dvar, deadshot, holding;
} hold_gates_t;

// Why the game's pmove doesn't hold the breath in this frame, in the order it asks, or "" when it does: from now on
// (its timer is 0) or still (it was held and the timer counts).
static const char *hold_refused(const hold_gates_t *gates) {
    if (gates->sights != 1.0f)
        return "the sights aren't all the way up";
    if (gates->weapon_class == Weapon_Class_Item)
        return "an item";
    if (!gates->steadies)
        return "the weapon isn't steadied by it";
    if (!gates->breath_button)
        return "the breath button isn't down";
    if (gates->quick_scope_ms > 0 && gates->quick_scope_dvar)
        return "the quick scope timer runs";
    if (gates->deadshot)
        return "Deadshot";
    if (!gates->holding && gates->breath_timer_ms != 0)
        return "the breath is still coming back";
    return "";
}

// What the game's hint function looks at before it draws "Hold [key] to steady" (Mac 0x101130494.., Windows
// 0x142074e3d..).
typedef struct {
    bool draw_dvar, alternate_aim, holding, eflag_0x4000, steadies, airburst;
    uint64_t visibility_bits;
    int32_t client_state, weapon_class;
    float sights;
} hint_tests_t;

// Why the game's hint function draws nothing, in the order it asks, or "" when it draws its hint.
static const char *hint_refused(const hint_tests_t *tests) {
    if (!tests->draw_dvar)
        return "cg_drawBreathHint is 0";
    if ((tests->visibility_bits & Hint_Visibility_Bits) != 0)
        return "a visibility bit";
    if (tests->client_state != 0)
        return "the client state";
    if (tests->alternate_aim)
        return "alternate aim params";
    if (tests->holding)
        return "the breath is held";
    if (tests->eflag_0x4000)
        return "eFlags 0x4000";
    if (tests->weapon_class == Weapon_Class_Item)
        return "an item";
    if (!tests->steadies)
        return "the weapon isn't steadied by holding the breath";
    if (tests->airburst)
        return "an airburst weapon";
    if (tests->sights != 1.0f)
        return "the sights aren't all the way up";
    return "";
}

static bool has_perk(const uint8_t *player_state, int perk) {
    return ((uint32_t)read_int(player_state + Ps_Perks + 4 * (perk / 32)) >> (perk % 32) & 1u) != 0;
}
static bool dvar_on(uint32_t hash) {
    const uint8_t *dvar = Dvar_FindByHash(hash);
    return dvar != NULL && Dvar_GetInt(dvar) != 0;
}

// what was last said for each local client, to say it again only when it changed (before the first line both reasons
// are NULL, which no answer is)
static struct {
    bool breath_button, sprint_button, holding, deadshot, hold_perk, alpha, renderer_alpha;
    const char *hold_refused, *hint_refused;
} said[Debug_Clients];

// A line for the game's hold and one for its hint, each when it changes (BO3MACFIX_BREATHDEBUG=1).
static void debug_gates(int local_client, const uint8_t *element, float renderer_alpha) {
    const uint8_t *cg = client_cg(local_client);
    if (cg == NULL || local_client >= Debug_Clients)
        return;
    const uint8_t *player_state = cg + Cg_PlayerState;
    const uint8_t *definition = BG_GetWeaponDef(BG_GetViewmodelWeapon(player_state));
    const uint8_t *buttons = key_buttons + (size_t)local_client * Key_Buttons_Size;
    hold_gates_t hold = {
        .sights = read_float(player_state + Ps_Sights),
        .weapon_class = read_int(definition + WeaponDef_Class),
        .quick_scope_ms = read_int(player_state + Ps_QuickScopeTimer),
        .breath_timer_ms = read_int(player_state + Ps_HoldBreathTimer),
        .steadies = definition[WeaponDef_Steadies] != 0,
        .breath_button = buttons[Key_Button_Breath + Key_Button_Active] != 0,
        .quick_scope_dvar = dvar_on(Quick_Scope_Dvar_Hash),
        .deadshot = has_perk(player_state, Perk_Deadshot),
        .holding = (player_state[Ps_WeaponFlags] & Weapon_Flag_Holding_Breath) != 0,
    };
    hint_tests_t hint = {
        .draw_dvar = dvar_on(Draw_Hint_Dvar_Hash),
        .alternate_aim = cg[Cg_AlternateAim] != 0,
        .holding = hold.holding,
        .eflag_0x4000 = (read_int(player_state + Ps_EFlags) & EFlag_0x4000) != 0,
        .steadies = hold.steadies,
        .airburst = definition[WeaponDef_Airburst] != 0,
        .visibility_bits = visibility_bits[local_client],
        .client_state = read_int(local_client_ui + (size_t)local_client * Local_Client_UI_Size + Local_Client_UI_Hint),
        .weapon_class = hold.weapon_class,
        .sights = hold.sights,
    };
    bool sprint_button = buttons[Key_Button_Sprint + Key_Button_Active] != 0;
    bool hold_perk = has_perk(player_state, Perk_HoldBreath);
    float alpha = read_float(element + Element_Alpha);
    const char *why_not_hold = hold_refused(&hold), *why_not_hint = hint_refused(&hint);
    typeof(said[0]) *last = &said[local_client];
    if (why_not_hold != last->hold_refused || hold.breath_button != last->breath_button ||
        sprint_button != last->sprint_button || hold.holding != last->holding || hold.deadshot != last->deadshot ||
        hold_perk != last->hold_perk)
        say("hold breath: local client %d, the game's pmove holds the breath: %s%s (breath button %d, sprint button "
               "%d, sights %.2f, weapon class %d steadied %d, quick scope timer %d ms with its dvar %d, Deadshot %d, Hold "
               "Breath perk %d (a breath lasts %d ms), breath timer %d ms, holding %d)\n", local_client,
               *why_not_hold != '\0' ? "no, " : "yes", why_not_hold, hold.breath_button, sprint_button,
               (double)hold.sights, hold.weapon_class, hold.steadies, hold.quick_scope_ms, hold.quick_scope_dvar,
               hold.deadshot, hold_perk, hold_perk ? 9500 : Gasp_After_Ms, hold.breath_timer_ms, hold.holding);
    if (why_not_hint != last->hint_refused || (alpha > 0.0f) != last->alpha ||
        (renderer_alpha > 0.0f) != last->renderer_alpha)
        say("hold breath: local client %d, the game's hint function draws its hint: %s%s (cg_drawBreathHint %d, "
               "visibility bits %#llx of %#llx, client state %d, alternate aim params %d; the element's alpha %.2f, "
               "the renderer's %.2f)\n", local_client, *why_not_hint != '\0' ? "no, " : "yes", why_not_hint,
               hint.draw_dvar, (unsigned long long)(hint.visibility_bits & Hint_Visibility_Bits),
               (unsigned long long)Hint_Visibility_Bits, hint.client_state, hint.alternate_aim, (double)alpha,
               (double)renderer_alpha);
    *last = (typeof(*last)){ hold.breath_button, sprint_button, hold.holding, hold.deadshot, hold_perk, alpha > 0.0f,
                             renderer_alpha > 0.0f, why_not_hold, why_not_hint };
}

// The render function setupHoldBreathHint gives an element: (the local client, the element, what the renderer hands
// every element, four floats, the UI's Lua state). The game draws its hint, then the status is T7Overcharged's.
install_hook_name(UIElement_RenderHoldBreathHint, void, int local_client, void *element, void *context, float first,
                  float second, float third, float fourth, void *lua) {
    orig_UIElement_RenderHoldBreathHint(local_client, element, context, first, second, third, fourth, lua);
    bool working = atomic_load_explicit(&started, memory_order_relaxed) && aae_plugin_loaded_lately();
    if (working)
        breath_status(local_client);
    if (debug) {
        static _Atomic bool ran[2];
        if (!atomic_exchange_explicit(&ran[working], true, memory_order_relaxed))
            printf(working ? "hold breath: the game's hold breath hint function ran for local client %d, "
                             "T7Overcharged's aim status is worked out there from now on\n"
                           : "hold breath: the game's hold breath hint function ran for local client %d, nothing "
                             "handed over: the loaded mod hasn't started T7Overcharged\n", local_client);
        debug_gates(local_client, element, fourth);
    }
}

// T7Overcharged's two dvars, with the name written into the dvar after it is registered, as T7Overcharged does
static void register_int(const char *name, int value, int min, int max, const char *description) {
    uint8_t *dvar = Dvar_RegisterInt(gsc_hash(name), name, value, min, max, 0, description);
    if (dvar != NULL)
        memcpy(dvar + Dvar_Name, &name, sizeof(name));
}
// a dvar as the game holds it, for the debug lines
static void print_dvar(const char *name) {
    const uint8_t *dvar = Dvar_FindByHash(gsc_hash(name));
    const char *own_name = NULL;
    if (dvar != NULL)
        memcpy(&own_name, dvar + Dvar_Name, sizeof(own_name));
    if (dvar == NULL)
        printf(" %s: none", name);
    else
        printf(" %s (named \"%s\"): kind %d, value %d, %d to %d", name, own_name != NULL ? own_name : "",
               read_int(dvar + Dvar_Type), read_int(dvar + Dvar_Value), read_int(dvar + Dvar_Domain),
               read_int(dvar + Dvar_Domain + 4));
}

void aae_breath_t7o_started(void) {
    if (!enabled || atomic_load_explicit(&started, memory_order_relaxed))
        return;
    register_int("holdBreathTimer", 0, 0, 5000, "");
    register_int("aim_status", 0, 0, 5, "aim type");
    atomic_store_explicit(&started, true, memory_order_relaxed);
    if (debug) {
        printf("hold breath: the mod started T7Overcharged, its dvars:");
        print_dvar("holdBreathTimer");
        print_dvar("aim_status");
        printf("\n");
    }
}

void aae_breath_init(uint64_t game_base_address, int enabled_setting) {
    enabled = enabled_setting;
    const char *env = getenv("BO3MACFIX_BREATHDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    if (!enabled) {
        if (debug)
            printf("hold breath: AAEHoldBreath is 0: the game's hold breath hint is left alone and T7Overcharged's "
                   "dvars aren't registered\n");
        return;
    }
    aae_plugin_init(game_base_address);
    cg_array = (uint8_t *const *)(game_base_address + ADDR_cgArray);
    local_client_count = (const int32_t *)(game_base_address + ADDR_LOCAL_CLIENT_COUNT);
    BG_GetViewmodelWeapon = (void *)(game_base_address + ADDR_BG_GetViewmodelWeapon);
    BG_GetWeaponDef = (void *)(game_base_address + ADDR_BG_GetWeaponDef);
    BG_WeaponOverlayReticle = (void *)(game_base_address + ADDR_BG_WeaponOverlayReticle);
    BG_WeaponHasZoomLevels = (void *)(game_base_address + ADDR_BG_WeaponHasZoomLevels);
    Com_LocalClient_GetControllerIndex = (void *)(game_base_address + ADDR_Com_LocalClient_GetControllerIndex);
    UIModel_ForController = (void *)(game_base_address + ADDR_UIModel_ForController);
    UIModel_Child = (void *)(game_base_address + ADDR_UIModel_Child);
    UIModel_SetInt = (void *)(game_base_address + ADDR_UIModel_SetInt);
    UIModel_SetString = (void *)(game_base_address + ADDR_UIModel_SetString);
    UIModel_ForceNotify = (void *)(game_base_address + ADDR_UIModel_ForceNotify);
    Dvar_RegisterInt = (void *)(game_base_address + ADDR_Dvar_RegisterInt);
    Dvar_SetFromStringByName = (void *)(game_base_address + ADDR_Dvar_SetFromStringByName);
    Dvar_FindByHash = (void *)(game_base_address + ADDR_Dvar_FindByHash);
    Dvar_GetInt = (void *)(game_base_address + ADDR_Dvar_GetInt);
    key_buttons = (const uint8_t *)(game_base_address + ADDR_KeyButtons);
    visibility_bits = (const uint64_t *)(game_base_address + ADDR_UIVisibilityBits);
    local_client_ui = (const uint8_t *)(game_base_address + ADDR_LocalClientUI);
    install_hook_UIElement_RenderHoldBreathHint((void *)(game_base_address + ADDR_UIElement_RenderHoldBreathHint));
    if (debug)
        printf("hold breath: the game's hold breath hint is hooked (AAEHoldBreath)\n");
}
