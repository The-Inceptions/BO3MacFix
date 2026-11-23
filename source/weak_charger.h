// WeakChargerWatts: a charger under so many watts counts as battery, so the battery settings stay on while the Mac runs
// from a charger too small to run the game at full speed and charge the battery too. See weak_charger.c.
#pragma once
#include <limits.h>
#include <stdbool.h>

// the setting's watts: 1 to this; anything else is off (0)
enum { Weak_Charger_Max_Watts = 300 };
// the most watts a charger's report is believed at (USB-C's largest is 240 W, Apple's 140 W); 0 or less, or more than
// this, is an unknown wattage
enum { Charger_Max_Known_Watts = 1000 };

// WeakChargerWatts as it applies: the value when it is 1 to 300, else 0 (off)
int weak_charger_setting(int value);

// whether a charger's reported watts are believable: 1 to Charger_Max_Known_Watts
bool weak_charger_watts_known(int watts);

// whether the Mac counts as running on battery for the battery settings: it runs on battery, or WeakChargerWatts
// (setting, as set: weak_charger_setting applies) is on and the charger's watts are known and under it. A charger of
// unknown wattage never counts as a small one.
bool weak_charger_counts_as_battery(bool on_battery, int setting, int charger_watts);

// what WeakChargerWatts' lines last said powers the Mac: source is Power_Battery, a charger's watts (0: unknown), or
// Power_Unsaid (nothing yet, or the setting off); battery is whether that counted as battery
enum { Power_Battery = -1, Power_Unsaid = INT_MIN };
typedef struct {
    int source;
    bool battery;
} power_said_t;
#define Power_Said_Start ((power_said_t){ Power_Unsaid, false })

// WeakChargerWatts' line after a look at the power source (gpu_tweaks.c's power watch), one when what powers the Mac
// changed and none while the setting is off:
typedef enum {
    Power_Line_None,
    Power_Line_Small,   // "on a N W charger, which counts as battery (WeakChargerWatts M)", with ", limiting the
                        // frame rate" in place of the old line ("running on battery, limiting the frame rate") when due
    Power_Line_Charger, // "plugged in (N W charger)", after the old line if it's due
    Power_Line_Unknown, // "plugged in (a charger of unknown wattage)", the same
    Power_Line_Battery, // "running on battery": off a charger, when the old line isn't due (off a small charger it
                        // never is: the counted state stays battery)
} power_line_t;

// which line, from the setting (as set), the power source, the charger's report (0 when not asked) and whether the old
// line is due (the counted state changed while BatteryFPS is set); *said is what was said before and becomes what this
// look says. At the start (*said Power_Said_Start) on battery nothing: BatteryFPS's own line says "on battery now".
power_line_t weak_charger_line(power_said_t *said, int setting, bool on_battery, int charger_watts, bool old_line);
