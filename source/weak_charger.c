// WeakChargerWatts: a charger under so many watts counts as battery. Measured on 2026-11-24 on the 16-inch MacBook Pro
// (wip/night/impl-weakcharger.md): on its 65 W USB-C charger, with the game at 120 fps the Mac drew 55 to 80 W, so the
// battery didn't charge, or drained, while plugged in; at the battery limit of 40 fps it drew 25 to 45 W and the
// battery charged. A plane's seat power is as weak or weaker. With the setting on, such a charger keeps the battery
// settings on (BatteryFPS, BatteryResolution, BatterySaver, IdleDisplaySleep 1, AAEEngineDefaults 2): gpu_tweaks.c's
// power watch stores this rule's answer where it stored "on battery". The charger's watts are macOS's
// (IOPSCopyExternalPowerAdapterDetails), asked only with the setting on and the Mac plugged in.
#include "weak_charger.h"

int weak_charger_setting(int value) {
    return value >= 1 && value <= Weak_Charger_Max_Watts ? value : 0;
}

bool weak_charger_watts_known(int watts) {
    return watts >= 1 && watts <= Charger_Max_Known_Watts;
}

bool weak_charger_counts_as_battery(bool on_battery, int setting, int charger_watts) {
    if (on_battery)
        return true;
    int limit = weak_charger_setting(setting);
    return limit > 0 && weak_charger_watts_known(charger_watts) && charger_watts < limit;
}

power_line_t weak_charger_line(power_said_t *said, int setting, bool on_battery, int charger_watts, bool old_line) {
    int limit = weak_charger_setting(setting);
    bool battery = weak_charger_counts_as_battery(on_battery, limit, charger_watts);
    int source = on_battery ? Power_Battery : weak_charger_watts_known(charger_watts) ? charger_watts : 0;
    power_said_t before = *said;
    *said = (power_said_t){ limit > 0 ? source : Power_Unsaid, battery };
    if (limit == 0 || (source == before.source && battery == before.battery))
        return Power_Line_None;
    if (!on_battery)
        return battery ? Power_Line_Small : source > 0 ? Power_Line_Charger : Power_Line_Unknown;
    // unplugged: said only off a charger (not at the start, nor when the setting comes on), where the old line isn't
    return before.source >= 0 && !old_line ? Power_Line_Battery : Power_Line_None;
}
