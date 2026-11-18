// AAEEngineDefaults (aae_engine_defaults.h): the gun's shadow resolution at the game's own values while All-around
// Enhancement raises it.
//
// What it is for (wip/night/perf-gpu2.md has the runs): with the mod and its ProMod option off, the mod's HUD script
// sets r_viewModelSunShadowResolution and r_viewModelSpotOmniShadowResolution to 4.0X at every match start (its ProMod
// function with no argument; no stock script or fastfile sets them, and the game's own values are 2.0X and 1.0X). The
// gun is then drawn into large tiles of the shadow atlas for the lamps near the player, each frame. Through this
// piece's own test switch (on and off every 8 s at a fixed spot, gcp6 savertest / kinosavertest, a busy Mac) the
// game's own values gave back about 0.2 ms of 15 ms a frame on the heavy custom map DEIMOS (nine pairs, +0.15 to
// -0.44) and about 0.45 ms of 8.1 on Kino at the 120 Hz limit (eight pairs, -0.03 to -0.75), with no difference seen
// on the gun or the arms in eight picture pairs (none of which shows a sharp gun shadow). Nothing else below the
// graphics menu's floor tried in those runs saved more than the runs' noise without a change in the picture, or
// else it was one of the menu's own values (ActorShadows). It lowers a raised value whoever raised it (the mod,
// another mod, the player's console): it doesn't ask which mod is loaded, because the saving is the same and a
// player who turns it on asked for the game's values.
//
// How: the values are looked at twice a second on the game thread and set again when they are above the game's own
// (the mod's HUD sets them at every match start and map restart), with the game's own Dvar_SetFromString and the
// internal source (0): the path the mod's own Engine.SetDvar takes, on the same thread. The renderer reads them once a
// frame on that thread; they pick a tile size in the shadow atlas, nothing is made or freed.
//
// Every client frame, nothing but a countdown is compared; every 16th frame the clock; twice a second, while the
// setting asks or a value is held, the two values are read straight from their dvars (their type, an enum, checked
// against the engine's own type field when each is first found). The engine is called only to find a dvar and to set
// one. Never through the game's Dvar_GetString: that gives an enum's name but, for any other type, the value's raw
// bytes as a text pointer (0x100fa04ad); the first version of this piece printed a float that way and crashed the game
// (gcp4_perfgpu_saveron, gcp5c: fault address 0x40800000, 4.0f's bits).
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aae_engine_defaults.h"
#include "gpu_tweaks.h"
#include "gsc_obj.h"
#include "offsets.h"

const engine_default_t aae_engine_defaults[] = {
    { "r_viewModelSpotOmniShadowResolution", 2 }, // the gun's shadow from spot and omni lights: 1.0X
    { "r_viewModelSunShadowResolution", 1 }, // from the sun: 2.0X
};
const int aae_engine_default_count = sizeof(aae_engine_defaults) / sizeof(aae_engine_defaults[0]);

bool aae_engine_defaults_active(int setting, bool on_battery) {
    return setting == 1 || (setting == 2 && on_battery);
}

engine_default_action_t aae_engine_default_step(const engine_default_t *dvar, engine_default_state_t *state,
                                                bool active, int current, int *value) {
    // something else set it since (the mod's HUD at a match start, the player's console, ProMod): not ours to put back
    if (state->held && current != dvar->wanted)
        state->held = false;
    if (active) {
        // only ever lowered: 4.0X (0) and 2.0X (1) are above the lamps' 1.0X (2); ProMod's 0.5X (3) is left alone
        if (current < dvar->wanted) {
            state->replaced = current; // not held here: a held value is the game's own, which isn't below it
            state->held = true;
            *value = dvar->wanted;
            return Engine_Default_Set;
        }
        return Engine_Default_Nothing;
    }
    if (state->held) {
        state->held = false;
        *value = state->replaced;
        return Engine_Default_Restore;
    }
    return Engine_Default_Nothing;
}

const char *aae_engine_default_text(int index, char *buffer, int size) {
    static const char *const scales[] = { "4.0X", "2.0X", "1.0X", "0.5X" };
    if (index >= 0 && index < (int)(sizeof(scales) / sizeof(scales[0])))
        return scales[index];
    snprintf(buffer, size, "%d", index);
    return buffer;
}

#pragma mark - the game's half

enum { Frames_Between_Clocks = 16 };

static void *(*Dvar_FindByHash)(uint32_t hash);
static void (*Dvar_SetFromString)(void *dvar, const char *value, int source);
static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}
static uint64_t (*clock_ns)(void) = now_ns; // the tests' own clock

static atomic_int setting_now = 0;
static bool ready = false;
static int test_seconds = 0; // BO3MACFIX_AAEENGINEDEFAULTSTEST: on and off in turn every this many seconds
static uint8_t *found[2];
static bool refused[2]; // not an enum dvar: never read or set
static engine_default_state_t states[2];
static int countdown = 0;
static uint64_t next_look = 0, test_start = 0;
static int last_active = -1;
_Static_assert(sizeof(aae_engine_defaults) / sizeof(aae_engine_defaults[0]) <= 2, "room for every dvar");

// With 2 the battery is asked at every look. The first ask starts BatteryFPS's power watch (gpu_tweaks.c), whose first
// check (IOPSCopyPowerSourcesInfo and NSProcessInfo, about 1.3 ms under Rosetta, measured by review-engine-defaults.md)
// would otherwise run on the game thread in a match frame when neither BatteryFPS nor LowPowerFPS has started it: so
// it is started here, at the library's start or when the settings page picks 2. After that an ask is a flag read.
static void start_battery_watch(int setting) {
    if (setting == 2)
        gpu_tweaks_on_battery();
}

void aae_engine_defaults_init(uint64_t base, int setting) {
    Dvar_FindByHash = (void *)(base + ADDR_Dvar_FindByHash);
    Dvar_SetFromString = (void *)(base + ADDR_Dvar_SetFromString);
    atomic_store(&setting_now, setting);
    start_battery_watch(setting);
    const char *env = getenv("BO3MACFIX_AAEENGINEDEFAULTSTEST");
    test_seconds = env != NULL ? atoi(env) : 0;
    ready = true;
    printf("AAEEngineDefaults: %d%s\n", setting, test_seconds > 0 ? ", testing: off and on in turn" : "");
}

void aae_engine_defaults_set(int setting) {
    atomic_store(&setting_now, setting);
    start_battery_watch(setting);
}

// the dvar's value, by index: the int32 at +0x28, once its type is known to be an enum (no engine call)
static int value_of(const uint8_t *dvar) {
    int32_t value;
    memcpy(&value, dvar + dvar_Value, sizeof(value));
    return value;
}

void aae_engine_defaults_frame(void) {
    if (!ready || --countdown > 0)
        return;
    countdown = Frames_Between_Clocks;
    uint64_t now = clock_ns();
    if (now < next_look)
        return;
    next_look = now + 500000000ull;
    int setting = atomic_load(&setting_now);
    bool active = aae_engine_defaults_active(setting, setting == 2 && gpu_tweaks_on_battery());
    if (test_seconds > 0 && active) {
        if (test_start == 0)
            test_start = now;
        if ((now - test_start) / 1000000000ull / test_seconds % 2 == 0)
            active = false;
    }
    if (active != last_active) {
        printf("AAEEngineDefaults: %s\n", active ? "on" : "off");
        last_active = active;
    }
    for (int i = 0; i < aae_engine_default_count; i++) {
        if (refused[i] || (!active && !states[i].held))
            continue;
        const engine_default_t *dvar = &aae_engine_defaults[i];
        if (found[i] == NULL) {
            found[i] = Dvar_FindByHash(gsc_hash(dvar->name)); // registered at the renderer's start, kept for good
            if (found[i] == NULL)
                continue;
            uint32_t type;
            memcpy(&type, found[i] + dvar_Type, sizeof(type));
            if (type != Engine_Defaults_Enum_Type) {
                refused[i] = true;
                printf("AAEEngineDefaults: %s is of type %u, not an enum: left alone\n", dvar->name, type);
                continue;
            }
        }
        int current = value_of(found[i]), value;
        engine_default_action_t action = aae_engine_default_step(dvar, &states[i], active, current, &value);
        if (action == Engine_Default_Nothing)
            continue;
        char text[16], before[16], after[16];
        snprintf(text, sizeof(text), "%d", value);
        Dvar_SetFromString(found[i], text, 0);
        printf("AAEEngineDefaults: %s %s -> %s%s\n", dvar->name, aae_engine_default_text(current, before, sizeof(before)),
               aae_engine_default_text(value_of(found[i]), after, sizeof(after)),
               action == Engine_Default_Restore ? " (back as it was)" : "");
    }
}
