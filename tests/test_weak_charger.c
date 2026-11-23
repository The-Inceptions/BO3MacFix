// Tests for weak_charger.c: when WeakChargerWatts makes a charger count as battery, and the setting's bounds.
#include <stdio.h>
#include "../source/weak_charger.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// on battery is battery, whatever the setting and whatever a charger's watts say (BO3MACFIX_ONBATTERY=1 with
// BO3MACFIX_CHARGERWATTS set, or a report left over from the charger just unplugged)
static void test_on_battery(void) {
    CHECK(weak_charger_counts_as_battery(true, 0, 0));
    CHECK(weak_charger_counts_as_battery(true, 100, 0));
    CHECK(weak_charger_counts_as_battery(true, 100, 65));
    CHECK(weak_charger_counts_as_battery(true, 100, 140));
    CHECK(weak_charger_counts_as_battery(true, 0, 65));
    CHECK(weak_charger_counts_as_battery(true, -5, -1));
}

// the setting off: plugged in is plugged in, whatever the charger (what every build before it did)
static void test_off(void) {
    CHECK(!weak_charger_counts_as_battery(false, 0, 65));
    CHECK(!weak_charger_counts_as_battery(false, 0, 1));
    CHECK(!weak_charger_counts_as_battery(false, 0, 0));
    CHECK(!weak_charger_counts_as_battery(false, 0, 140));
}

// on: a charger under the setting counts as battery; one at it or over it doesn't
static void test_small_equal_larger(void) {
    CHECK(weak_charger_counts_as_battery(false, 100, 65));  // tonight's charger
    CHECK(weak_charger_counts_as_battery(false, 70, 65));
    CHECK(weak_charger_counts_as_battery(false, 100, 99));  // one under
    CHECK(weak_charger_counts_as_battery(false, 100, 1));   // the smallest known
    CHECK(!weak_charger_counts_as_battery(false, 100, 100)); // at it
    CHECK(!weak_charger_counts_as_battery(false, 70, 70));
    CHECK(!weak_charger_counts_as_battery(false, 100, 101)); // one over
    CHECK(!weak_charger_counts_as_battery(false, 100, 140)); // the 16-inch's own charger
    CHECK(!weak_charger_counts_as_battery(false, 70, 96));
    CHECK(weak_charger_counts_as_battery(false, 300, 240));  // the largest setting, USB-C's largest charger
    CHECK(weak_charger_counts_as_battery(false, 300, 299));
    CHECK(!weak_charger_counts_as_battery(false, 300, 300));
    CHECK(!weak_charger_counts_as_battery(false, 1, 1));     // the smallest setting: nothing is under 1 W
}

// a charger of unknown wattage (no adapter details: 0; a negative or an absurd report) is never a small one
static void test_unknown(void) {
    CHECK(!weak_charger_counts_as_battery(false, 100, 0));
    CHECK(!weak_charger_counts_as_battery(false, 100, -1));
    CHECK(!weak_charger_counts_as_battery(false, 300, -65));
    CHECK(!weak_charger_counts_as_battery(false, 300, 1001));
    CHECK(!weak_charger_counts_as_battery(false, 300, 100000));
    CHECK(!weak_charger_watts_known(0));
    CHECK(!weak_charger_watts_known(-1));
    CHECK(!weak_charger_watts_known(1001));
    CHECK(weak_charger_watts_known(1));
    CHECK(weak_charger_watts_known(65));
    CHECK(weak_charger_watts_known(1000));
}

// the setting's bounds: 1 to 300 are watts, anything else is off, in the rule too
static void test_bounds(void) {
    CHECK(weak_charger_setting(0) == 0);
    CHECK(weak_charger_setting(1) == 1);
    CHECK(weak_charger_setting(70) == 70);
    CHECK(weak_charger_setting(100) == 100);
    CHECK(weak_charger_setting(300) == 300);
    CHECK(weak_charger_setting(301) == 0);
    CHECK(weak_charger_setting(-1) == 0);
    CHECK(weak_charger_setting(-100) == 0);
    CHECK(weak_charger_setting(100000) == 0);
    CHECK(!weak_charger_counts_as_battery(false, 301, 65));  // over the range: off, not "under 301 W"
    CHECK(!weak_charger_counts_as_battery(false, 1000, 65));
    CHECK(!weak_charger_counts_as_battery(false, -100, 65)); // negative: off
    CHECK(!weak_charger_counts_as_battery(false, -100, -200));
}


// the lines: what the power watch says, one look after another (said carries over as in gpu_tweaks.c)
static void test_lines_start(void) {
    power_said_t said = Power_Said_Start;
    // the start on a small charger: its line, no old line due (the first look says none)
    CHECK(weak_charger_line(&said, 100, false, 65, false) == Power_Line_Small);
    CHECK(said.source == 65 && said.battery);
    // the same charger at the next looks: nothing (one line per change, not one per look)
    CHECK(weak_charger_line(&said, 100, false, 65, false) == Power_Line_None);
    CHECK(weak_charger_line(&said, 100, false, 65, false) == Power_Line_None);

    said = Power_Said_Start; // the start on a bigger charger: "plugged in (140 W charger)", once
    CHECK(weak_charger_line(&said, 100, false, 140, false) == Power_Line_Charger);
    CHECK(weak_charger_line(&said, 100, false, 140, false) == Power_Line_None);
    said = Power_Said_Start; // at the setting's watts: plugged in too
    CHECK(weak_charger_line(&said, 100, false, 100, false) == Power_Line_Charger);

    said = Power_Said_Start; // the start on a charger of unknown wattage
    CHECK(weak_charger_line(&said, 100, false, 0, false) == Power_Line_Unknown);
    CHECK(weak_charger_line(&said, 100, false, 0, false) == Power_Line_None);
    said = Power_Said_Start;
    CHECK(weak_charger_line(&said, 100, false, -3, false) == Power_Line_Unknown);
    said = Power_Said_Start;
    CHECK(weak_charger_line(&said, 100, false, 5000, false) == Power_Line_Unknown);

    said = Power_Said_Start; // the start on battery: nothing (BatteryFPS's own line says "on battery now")
    CHECK(weak_charger_line(&said, 100, true, 0, false) == Power_Line_None);
    CHECK(said.source == Power_Battery && said.battery);
}

static void test_lines_changes(void) {
    power_said_t said = Power_Said_Start;
    CHECK(weak_charger_line(&said, 100, true, 0, false) == Power_Line_None);    // on battery
    CHECK(weak_charger_line(&said, 100, false, 65, false) == Power_Line_Small); // plugged into 65 W: still battery
    CHECK(weak_charger_line(&said, 100, true, 0, false) == Power_Line_Battery); // unplugged: no old line, so this one
    CHECK(weak_charger_line(&said, 100, false, 140, true) == Power_Line_Charger); // 140 W: after "plugged in, ..."
    CHECK(weak_charger_line(&said, 100, true, 0, true) == Power_Line_None);    // unplugged: the old line says it
    CHECK(weak_charger_line(&said, 100, false, 140, false) == Power_Line_Charger); // without BatteryFPS: no old line
    CHECK(weak_charger_line(&said, 100, true, 0, false) == Power_Line_Battery); // ...and unplugged: no old line either
    CHECK(weak_charger_line(&said, 100, false, 65, false) == Power_Line_Small);
    CHECK(weak_charger_line(&said, 100, false, 60, false) == Power_Line_Small); // the report changed: said again
    CHECK(said.source == 60);
    CHECK(weak_charger_line(&said, 100, false, 140, true) == Power_Line_Charger); // small to big, without unplugging
    CHECK(weak_charger_line(&said, 100, false, 65, true) == Power_Line_Small);    // big to small: the merged line
    CHECK(weak_charger_line(&said, 100, false, 0, true) == Power_Line_Unknown);   // small to unknown
    CHECK(weak_charger_line(&said, 100, true, 0, false) == Power_Line_Battery);   // unplugged off it, no old line due
}

// the setting changed while the watch runs (the settings page): off says nothing and forgets what was said, on says
// the charger again; the same charger at a new setting is said when it no longer counts the same
static void test_lines_setting(void) {
    power_said_t said = Power_Said_Start;
    CHECK(weak_charger_line(&said, 100, false, 65, false) == Power_Line_Small);
    CHECK(weak_charger_line(&said, 0, false, 0, true) == Power_Line_None); // off: the old "plugged in" line only
    CHECK(said.source == Power_Unsaid);
    CHECK(weak_charger_line(&said, 0, false, 0, false) == Power_Line_None);
    CHECK(weak_charger_line(&said, 0, true, 0, true) == Power_Line_None);  // off, on battery: the old line only
    CHECK(weak_charger_line(&said, 0, false, 0, true) == Power_Line_None);
    CHECK(weak_charger_line(&said, 100, false, 65, true) == Power_Line_Small); // on again: the merged line
    CHECK(weak_charger_line(&said, 70, false, 65, false) == Power_Line_None);  // 100 to 70: 65 W still counts
    CHECK(weak_charger_line(&said, 60, false, 65, true) == Power_Line_Charger); // 70 to 60: it doesn't any more
    CHECK(weak_charger_line(&said, 100, false, 65, true) == Power_Line_Small);
    CHECK(weak_charger_line(&said, 500, false, 65, true) == Power_Line_None);   // out of range: off
    CHECK(said.source == Power_Unsaid);
    said = Power_Said_Start;
    CHECK(weak_charger_line(&said, 100, true, 0, false) == Power_Line_None);
    CHECK(weak_charger_line(&said, 0, true, 0, false) == Power_Line_None);
    CHECK(weak_charger_line(&said, 100, true, 0, false) == Power_Line_None);    // on again, on battery: nothing
}

int main(void) {
    test_on_battery();
    test_off();
    test_small_equal_larger();
    test_unknown();
    test_bounds();
    test_lines_start();
    test_lines_changes();
    test_lines_setting();
    if (failures > 0) {
        printf("test_weak_charger: %d failed\n", failures);
        return 1;
    }
    printf("test_weak_charger: all passed\n");
    return 0;
}
