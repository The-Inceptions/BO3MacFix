#pragma once
// AAEEngineDefaults: the gun's shadow resolution at the game's own values while All-around Enhancement raises it
// (aae_engine_defaults.c). 0 off, 1 always, 2 only while the Mac runs on battery.
//
// The mod's HUD sets r_viewModelSunShadowResolution and r_viewModelSpotOmniShadowResolution to 4.0X at every match
// start (its ProMod option off); the game's own values are 2.0X and 1.0X. A value is only ever lowered to the game's
// own (the mod's ProMod, which sets 0.5X, is left alone), and a value set here goes back when the setting stops
// asking, unless something else changed it meanwhile.
#include <stdbool.h>
#include <stdint.h>

// the engine's enum dvar type (the dword at dvar_t + 0x1c, offsets.h's dvar_Type): these two dvars are enums; their
// value (the index, an int32 at dvar_t + 0x28) is read straight from the dvar once its type is checked
enum { Engine_Defaults_Enum_Type = 7 };

typedef struct {
    const char *name; // the dvar (found by the engine's hash of the name)
    int wanted; // the game's own value, by index (a higher index is a smaller shadow)
} engine_default_t;

typedef struct {
    bool held; // set here, and it still had that value when last looked at
    int replaced; // the value it had before
} engine_default_state_t;

typedef enum { Engine_Default_Nothing, Engine_Default_Set, Engine_Default_Restore } engine_default_action_t;

extern const engine_default_t aae_engine_defaults[];
extern const int aae_engine_default_count;

// whether the setting asks for the game's values now: 1 always, 2 on battery
bool aae_engine_defaults_active(int setting, bool on_battery);

// one dvar, given its value now: Engine_Default_Set when it should become *value (it is above the game's own value
// while active), Engine_Default_Restore when *value is what it had before this set it (not active any more), else
// nothing. A held value that changed meanwhile is let go, not put back.
engine_default_action_t aae_engine_default_step(const engine_default_t *dvar, engine_default_state_t *state,
                                                bool active, int current, int *value);

// a shadow resolution's index as the log shows it: 4.0X, 2.0X, 1.0X, 0.5X, or the number
const char *aae_engine_default_text(int index, char *buffer, int size);

// the game's half: reads the setting (also BO3MACFIX_AAEENGINEDEFAULTS) and BO3MACFIX_AAEENGINEDEFAULTSTEST
void aae_engine_defaults_init(uint64_t base, int setting);
// from the settings page
void aae_engine_defaults_set(int setting);
// each client frame, on the game thread (CL_SetCGameTime's hook)
void aae_engine_defaults_frame(void);
