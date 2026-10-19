// Tests for display_policy.c: which display the game uses, the resolution and refresh rate it gets there,
// displays.ini, telling displays apart, the debounce, and ConfineCursor's soft edge. No game or display needed.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../source/display_policy.h"

#define Builtin_UUID "37D8832A-2D66-02CA-B9F7-8F30A301B230"
#define Gaming_UUID "A5B77482-F6CF-4007-8E6B-3044BFC95B55"
#define TV_UUID "CB214AE0-3710-4F1F-8869-3AF9C23E6F65"

static const DisplayInfo builtin = { .id = 1, .uuid = Builtin_UUID, .key = Builtin_UUID, .name = "Built-in Retina Display",
                                     .builtin = 1, .main = 1, .w = 1728, .h = 1117, .pixel_w = 3456, .pixel_h = 2234,
                                     .native_w = 3456, .native_h = 2234, .scale = 2.0, .refresh = 120, .variable_refresh = 1,
                                     .notch = 32, .vendor = 1552, .model = 41011 };
static const DisplayInfo gaming = { .id = 2, .uuid = Gaming_UUID, .key = Gaming_UUID, .name = "Gaming monitor", .x = 1728,
                                    .w = 1920, .h = 1080, .pixel_w = 3840, .pixel_h = 2160, .native_w = 3840, .native_h = 2160,
                                    .scale = 2.0, .refresh = 240, .variable_refresh = 1, .vendor = 1138, .model = 1700,
                                    .serial = 777 };
static const DisplayInfo tv = { .id = 3, .uuid = TV_UUID, .key = TV_UUID, .name = "TV", .x = -1920, .w = 1920, .h = 1080,
                                .pixel_w = 1920, .pixel_h = 1080, .native_w = 1920, .native_h = 1080, .scale = 1.0, .refresh = 60,
                                .vendor = 19501, .model = 2 };

static RememberedDisplay shape(const DisplayInfo *display) {
    RememberedDisplay out;
    display_policy_shape(display, &out);
    return out;
}

// another of the same kind of monitor: a serial-less XB273K GP with its own UUID
static DisplayInfo another(const DisplayInfo *display, uint32_t id, const char *uuid) {
    DisplayInfo out = *display;
    out.id = id;
    out.main = 0;
    strcpy(out.uuid, uuid);
    strcpy(out.key, uuid);
    return out;
}

static void test_target(void) {
    DisplayInfo all[] = { builtin, gaming, tv };
    DisplayStore store = { 0 };
    assert(display_policy_target(all, 3, 0, &store) == 0);              // Main display
    assert(display_policy_target(all, 3, 1, &store) == 0);              // Built-in
    assert(display_policy_target(all, 3, 2, &store) == 1);              // External: the fastest one
    strcpy(store.last_external, TV_UUID);
    assert(display_policy_target(all, 3, 2, &store) == 2);              // External: the last one used, when it's there
    strcpy(store.last_external, Builtin_UUID);
    assert(display_policy_target(all, 3, 2, &store) == 1);              // a built-in is never "the last external"

    // the gaming monitor is the main display (lid closed, or set as main)
    DisplayInfo desk[] = { gaming, tv };
    desk[0].main = 1;
    strcpy(store.last_external, TV_UUID);
    assert(display_policy_target(desk, 2, 0, &store) == 0);
    assert(display_policy_target(desk, 2, 1, &store) == 0);             // no built-in: the main display
    assert(display_policy_target(desk, 2, 2, &store) == 1);

    DisplayInfo laptop[] = { builtin };
    assert(display_policy_target(laptop, 1, 2, &store) == 0);           // no external: the built-in
    assert(display_policy_target(NULL, 0, 0, &store) == -1);

    // the same refresh rate: the one with more pixels
    DisplayStore none = { 0 };
    DisplayInfo two[] = { builtin, tv, another(&tv, 4, "11111111-0000-0000-0000-000000000004") };
    two[2].native_w = 3840;
    two[2].native_h = 2160;
    assert(display_policy_target(two, 3, 2, &none) == 2);
}

static void test_chosen(void) {
    DisplayInfo all[] = { builtin, gaming, tv };
    DisplayStore store = { 0 };
    display_store_add(&store, &tv);
    strcpy(store.chosen, TV_UUID);
    strcpy(store.chosen_name, tv.name);
    assert(display_policy_target(all, 3, 3, &store) == 2);              // the chosen display, though it's slower

    // not connected: the External rule, then the built-in, then the main display
    DisplayInfo no_tv[] = { builtin, gaming };
    assert(display_policy_target(no_tv, 2, 3, &store) == 1);
    DisplayInfo laptop[] = { builtin };
    assert(display_policy_target(laptop, 1, 3, &store) == 0);
    DisplayInfo desk[] = { gaming };
    desk[0].main = 1;
    assert(display_policy_target(desk, 1, 3, &store) == 0);

    // nothing chosen yet: as External
    DisplayStore empty = { 0 };
    empty.count = 0;
    assert(display_policy_target(all, 3, 3, &empty) == 1);

    // the chosen monitor on another port (another UUID, the same serial)
    DisplayStore monitor = { 0 };
    display_store_add(&monitor, &gaming);
    strcpy(monitor.chosen, Gaming_UUID);
    DisplayInfo moved[] = { builtin, tv, another(&gaming, 9, "22222222-0000-0000-0000-000000000009") };
    assert(display_policy_target(moved, 3, 3, &monitor) == 2);

    // the chosen TV mirrors the built-in (so it's not in the list): the game goes where its picture shows on the TV,
    // the built-in, not to the gaming monitor by the External rule
    MirrorInfo tv_mirrors_builtin = { .key = TV_UUID, .primary = 1 };
    assert(display_policy_target_mirrored(no_tv, 2, 3, &store, &tv_mirrors_builtin, 1) == 0);
    assert(display_policy_target_mirrored(no_tv, 2, 3, &store, NULL, 0) == 1);
    // mirroring a display that isn't in the list either, or another display mirroring: the rules as before
    MirrorInfo elsewhere = { .key = TV_UUID, .primary = 42 }, other = { .key = Gaming_UUID, .primary = 1 };
    assert(display_policy_target_mirrored(no_tv, 2, 3, &store, &elsewhere, 1) == 1);
    assert(display_policy_target_mirrored(no_tv, 2, 3, &store, &other, 1) == 1);
    // connected and not mirroring: the chosen display; the other rules don't look at mirrors
    assert(display_policy_target_mirrored(all, 3, 3, &store, &tv_mirrors_builtin, 1) == 2);
    assert(display_policy_target_mirrored(no_tv, 2, 2, &store, &tv_mirrors_builtin, 1) == 1);
}

static void test_choices(void) {
    DisplayInfo xb = { .id = 5, .uuid = "33333333-0000-0000-0000-000000000005", .key = "33333333-0000-0000-0000-000000000005",
                       .name = "XB273K GP", .native_w = 3840, .native_h = 2160, .refresh = 120, .vendor = 1138, .model = 1800 };
    DisplayInfo all[] = { builtin, xb, another(&xb, 6, "33333333-0000-0000-0000-000000000006") };
    int values[8];
    char labels[8][64];
    int n = display_policy_choices(all, 3, NULL, values, labels, 8);
    assert(n == 6);
    assert(values[0] == 0 && strcmp(labels[0], "Main display") == 0);
    assert(values[1] == 1 && strcmp(labels[1], "Built-in") == 0);
    assert(values[2] == 2 && strcmp(labels[2], "External") == 0);
    assert(values[3] == 100 && strcmp(labels[3], "Built-in Retina Display") == 0);
    assert(values[4] == 101 && strcmp(labels[4], "XB273K GP") == 0);
    assert(values[5] == 102 && strcmp(labels[5], "XB273K GP (2)") == 0);
    assert(display_policy_choices(all, 3, NULL, values, labels, 4) == 4 && values[3] == 100);

    // what the page shows, and what settings.ini keeps
    DisplayStore store = { 0 };
    display_store_add(&store, &all[2]);
    strcpy(store.chosen, all[2].key);
    assert(display_policy_current(2, all, 3, NULL, &store) == 2);
    assert(display_policy_current(3, all, 3, NULL, &store) == 102);
    DisplayInfo laptop[] = { builtin, tv };
    assert(display_policy_current(3, laptop, 2, NULL, &store) == 3);          // the chosen one isn't connected
    // without it, the other serial-less XB273K GP (unknown to displays.ini) can't be told apart from it
    assert(display_policy_current(3, all, 2, NULL, &store) == 101);
    assert(display_policy_persisted("GameDisplay", 102) == 3);
    assert(display_policy_persisted("GameDisplay", 2) == 2);
    assert(display_policy_persisted("ConfineCursor", 1) == 1);

    // the values stay each display's for the session: a page that listed [built-in, XB, XB (2)] still picks the XB it
    // showed after the first XB is unplugged and a TV plugged in
    DisplaySlots table = { 0 };
    int slots[4];
    display_slots_update(&table, all, 3, slots);
    assert(slots[0] == 0 && slots[1] == 1 && slots[2] == 2);
    DisplayInfo later[] = { builtin, all[2], tv };
    display_slots_update(&table, later, 3, slots);
    assert(slots[0] == 0 && slots[1] == 2 && slots[2] == 3);
    n = display_policy_choices(later, 3, slots, values, labels, 8);
    assert(n == 6 && values[3] == 100 && values[4] == 102 && values[5] == 103 && strcmp(labels[5], "TV") == 0);
    assert(strcmp(display_slots_pick(&table, 102)->key, all[2].key) == 0);
    assert(strcmp(display_slots_pick(&table, 101)->key, all[1].key) == 0);  // unplugged, but still the one shown
    assert(display_slots_pick(&table, 104) == NULL && display_slots_pick(&table, 99) == NULL);
    assert(display_policy_current(3, later, 3, slots, &store) == 102);
    // the first XB back: its old value
    DisplayInfo back[] = { builtin, all[1], all[2], tv };
    display_slots_update(&table, back, 4, slots);
    assert(slots[1] == 1 && slots[2] == 2 && slots[3] == 3);

    // all slots taken: a new display takes the first slot whose display isn't connected
    DisplaySlots full = { 0 };
    DisplayInfo one = tv;
    for (int i = 0; i < Pick_Slots; i++) {
        snprintf(one.key, sizeof(one.key), "SLOT-%d", i);
        display_slots_update(&full, &one, 1, slots);
        assert(slots[0] == i);
    }
    DisplayInfo pair[] = { tv, tv };
    strcpy(pair[0].key, "SLOT-0");
    strcpy(pair[1].key, "SLOT-NEW");
    display_slots_update(&full, pair, 2, slots);
    assert(slots[0] == 0 && slots[1] == 1 && full.count == Pick_Slots && strcmp(display_slots_pick(&full, 101)->key, "SLOT-NEW") == 0);
}

static void test_identity(void) {
    // two identical monitors that macOS gave the same UUID: the second is keyed by its unit number
    DisplayInfo pair[] = { gaming, gaming };
    pair[1].id = 7;
    pair[1].unit = 3;
    display_policy_keys(pair, 2);
    assert(strcmp(pair[0].key, Gaming_UUID) == 0 && strcmp(pair[1].key, Gaming_UUID "#3") == 0);

    // the gaming monitor on another port: a new UUID, the same vendor, model and serial
    DisplayStore store = { 0 };
    RememberedDisplay *old = display_store_add(&store, &gaming);
    old->width = 3840;
    old->height = 2160;
    old->refresh = 240;
    strcpy(store.last_external, Gaming_UUID);
    strcpy(store.chosen, Gaming_UUID);
    strcpy(store.last_display, Gaming_UUID);
    DisplayInfo now[] = { builtin, another(&gaming, 9, "22222222-0000-0000-0000-000000000009") };
    char from[64];
    assert(!display_store_adopt(&store, now, 2, 0, from, sizeof(from)));   // no match for the built-in
    assert(display_store_adopt(&store, now, 2, 1, from, sizeof(from)) && strcmp(from, Gaming_UUID) == 0);
    RememberedDisplay *adopted = display_store_find(&store, now[1].key);
    assert(adopted != NULL && adopted->width == 3840 && adopted->height == 2160 && adopted->refresh == 240);
    assert(strcmp(store.last_external, now[1].key) == 0 && strcmp(store.chosen, now[1].key) == 0 &&
           strcmp(store.last_display, now[1].key) == 0);
    assert(!display_store_adopt(&store, now, 2, 1, from, sizeof(from)));   // known now

    // serial-less: the same vendor, model, name and native size, if exactly one remembered display is like it
    DisplayInfo xb = { .id = 5, .uuid = "33333333-0000-0000-0000-000000000005", .key = "33333333-0000-0000-0000-000000000005",
                       .name = "XB273K GP", .native_w = 3840, .native_h = 2160, .refresh = 120, .vendor = 1138, .model = 1800 };
    DisplayStore one = { 0 };
    display_store_add(&one, &xb)->width = 2560;
    DisplayInfo plugged[] = { builtin, another(&xb, 8, "44444444-0000-0000-0000-000000000008") };
    assert(display_store_adopt(&one, plugged, 2, 1, from, sizeof(from)) && strcmp(from, xb.key) == 0);
    assert(display_store_find(&one, plugged[1].key)->width == 2560);

    // two like it: no telling which, so no memory
    DisplayStore two = { 0 };
    display_store_add(&two, &xb);
    DisplayInfo xb2 = another(&xb, 6, "33333333-0000-0000-0000-000000000006");
    display_store_add(&two, &xb2);
    assert(!display_store_adopt(&two, plugged, 2, 1, from, sizeof(from)));

    // the one like it is connected right now, so this is another monitor
    DisplayStore connected = { 0 };
    display_store_add(&connected, &xb);
    DisplayInfo both[] = { xb, another(&xb, 8, "44444444-0000-0000-0000-000000000008") };
    assert(!display_store_adopt(&connected, both, 2, 1, from, sizeof(from)));

    // finding a remembered display that isn't under its key: by serial, else the only one of its kind
    DisplayStore remembered = { 0 };
    display_store_add(&remembered, &gaming);
    display_store_add(&remembered, &xb);
    DisplayInfo elsewhere[] = { builtin, another(&gaming, 9, "22222222-0000-0000-0000-000000000009"),
                                another(&xb, 8, "44444444-0000-0000-0000-000000000008") };
    assert(display_policy_find_remembered(elsewhere, 3, &remembered, Gaming_UUID) == 1);
    assert(display_policy_find_remembered(elsewhere, 3, &remembered, xb.key) == 2);
    assert(display_policy_find_remembered(elsewhere, 3, &remembered, TV_UUID) == -1);
    assert(display_policy_find_remembered(elsewhere, 3, &remembered, Builtin_UUID) == 0);
}

static void test_moving(void) {
    RememberedDisplay from = shape(&builtin), memory = shape(&gaming);
    GameValues now = { 2624, 1696, 120, 120 };
    DisplayDecision d;

    // nothing remembered for the gaming monitor: as many pixels at its shape, its refresh rate, and the 120 cap follows
    display_policy_decide(&from, &now, &gaming, NULL, 1, &d);
    printf("built-in 2624x1696 at 120 Hz -> %s %dx%d at %d Hz, MaxFPS %d (%s)\n", gaming.name, d.width, d.height,
           d.refresh, d.max_fps, d.why);
    assert(d.display == gaming.id && d.moved);
    assert(d.width == 2816 && d.height == 1584 && d.refresh == 240 && d.max_fps == 240);

    // 2336x1510: about 2560x1440 on the 4K panel (MetalFX takes it up to 4K), the whole panel on a 1x 1080p display
    GameValues smaller = { 2336, 1510, 120, 120 };
    display_policy_decide(&from, &smaller, &gaming, NULL, 1, &d);
    assert(d.width == 2504 && d.height == 1408);
    display_policy_decide(&from, &smaller, &tv, NULL, 1, &d);
    assert(d.width == 1920 && d.height == 1080 && d.refresh == 60 && d.max_fps == 60);

    // a 360 Hz monitor: RefreshRate stops at 240, the cap follows all the way
    DisplayInfo fast = gaming;
    fast.refresh = 360;
    display_policy_decide(&from, &now, &fast, NULL, 1, &d);
    assert(d.refresh == 240 && d.max_fps == 360);

    // remembered values win; the cap follows the display's refresh rate
    memory.width = 2560;
    memory.height = 1440;
    memory.refresh = 144;
    display_policy_decide(&from, &now, &gaming, &memory, 1, &d);
    assert(d.width == 2560 && d.height == 1440 && d.refresh == 144 && d.max_fps == 240);

    // the cap is compared with the old display's refresh rate, not the game's RefreshRate
    GameValues slow = { 2624, 1696, 60, 120 };
    display_policy_decide(&from, &slow, &gaming, NULL, 1, &d);
    assert(d.refresh == 240 && d.max_fps == 240);
    GameValues capped_at_rate = { 2624, 1696, 60, 60 };
    display_policy_decide(&from, &capped_at_rate, &gaming, NULL, 1, &d);
    assert(d.max_fps == -1);

    // MatchRefreshRate off: the refresh rate and the cap stay
    display_policy_decide(&from, &now, &gaming, NULL, 0, &d);
    assert(d.width == 2816 && d.refresh == 0 && d.max_fps == -1);

    // a cap that isn't the old refresh rate, or no cap, is the user's choice
    GameValues uncapped = { 2624, 1696, 120, 0 }, custom = { 2624, 1696, 120, 90 };
    display_policy_decide(&from, &uncapped, &gaming, NULL, 1, &d);
    assert(d.max_fps == -1);
    display_policy_decide(&from, &custom, &gaming, NULL, 1, &d);
    assert(d.max_fps == -1);

    // and back to the built-in at what it had
    RememberedDisplay back = shape(&builtin), monitor = shape(&gaming);
    back.width = 2624;
    back.height = 1696;
    back.refresh = 120;
    GameValues there = { 2816, 1584, 240, 240 };
    display_policy_decide(&monitor, &there, &builtin, &back, 1, &d);
    assert(d.display == builtin.id && d.width == 2624 && d.height == 1696 && d.refresh == 120 && d.max_fps == 120);

    // the 1x TV: 1920x1080 is all it has, and it's 60 Hz
    GameValues half = { 1920, 1080, 240, 240 };
    display_policy_decide(&monitor, &half, &tv, NULL, 1, &d);
    assert(d.width == 1920 && d.height == 1080 && d.refresh == 60 && d.max_fps == 60);
}

static void test_refresh(void) {
    assert(display_policy_refresh(120, 0) == 120);
    assert(display_policy_refresh(0, 144) == 144);
    assert(display_policy_refresh(2500, 60) == 60);   // the virtual display's 2500 frames per second
    assert(display_policy_refresh(2500, 0) == 60);
    assert(display_policy_refresh(0, 0) == 60);
    assert(display_policy_refresh(23, 600) == 60);

    // a refresh rate the game couldn't use isn't a change
    DisplayInfo bogus = tv;
    bogus.refresh = 2500;
    assert(!display_policy_changed(&tv, &bogus));
    RememberedDisplay from = shape(&builtin);
    GameValues now = { 2624, 1696, 120, 1000 };
    DisplayDecision d;
    display_policy_decide(&from, &now, &bogus, NULL, 1, &d);
    assert(d.refresh <= Game_Refresh_Max && d.max_fps <= Game_MaxFPS_Max);
}

static void test_same_display_changed(void) {
    RememberedDisplay before = shape(&gaming);
    DisplayInfo after = gaming;
    after.refresh = 120;
    DisplayDecision d;

    GameValues at_max = { 2912, 1640, 240, 240 };
    display_policy_decide(&before, &at_max, &after, NULL, 1, &d);
    assert(!d.moved && d.width == 0 && d.refresh == 120 && d.max_fps == 120);

    GameValues own = { 2912, 1640, 144, 144 };
    display_policy_decide(&before, &own, &after, NULL, 1, &d);
    assert(!d.moved && d.width == 0 && d.refresh == 0 && d.max_fps == -1);

    assert(!display_policy_changed(&gaming, &gaming));
    assert(display_policy_changed(&gaming, &after));
}

static void test_launch(void) {
    DisplayInfo all[] = { builtin, gaming };
    GameValues now = { 2624, 1696, 120, 120 };
    DisplayStore store = { 0 };
    DisplayDecision d;

    // last on the built-in: config.ini's values are the built-in's
    RememberedDisplay *entry = display_store_add(&store, &builtin);
    assert(entry != NULL);
    strcpy(store.last_display, Builtin_UUID);
    assert(!display_policy_launch(&store, all, 2, &builtin, &now, 1, 1, &d));
    assert(display_policy_launch(&store, all, 2, &gaming, &now, 1, 1, &d));
    assert(d.width == 2816 && d.height == 1584 && d.refresh == 240 && d.max_fps == 240);

    // no displays.ini yet: 2624x1696 has the MacBook's shape, so that's where it's from
    DisplayStore empty = { 0 };
    assert(!display_policy_launch(&empty, all, 2, &builtin, &now, 1, 1, &d));
    assert(display_policy_launch(&empty, all, 2, &gaming, &now, 1, 1, &d) && d.width == 2816);

    // 1920x1080 has the monitor's shape
    GameValues wide = { 1920, 1080, 240, 0 };
    assert(!display_policy_launch(&empty, all, 2, &gaming, &wide, 1, 1, &d));
    assert(display_policy_launch(&empty, all, 2, &builtin, &wide, 1, 1, &d));
    assert(d.width == 1792 && d.height == 1160 && d.refresh == 120 && d.max_fps == -1);

    // a shape that fits nothing: keep config.ini's values
    GameValues square = { 1024, 1024, 60, 0 };
    assert(!display_policy_launch(&empty, all, 2, &gaming, &square, 1, 1, &d));

    // remembered values, and DisplayMemory off ignoring them
    RememberedDisplay *monitor = display_store_add(&store, &gaming);
    monitor->width = 3840;
    monitor->height = 2160;
    monitor->refresh = 240;
    assert(display_policy_launch(&store, all, 2, &gaming, &now, 1, 1, &d) && d.width == 3840 && d.height == 2160);
    assert(display_policy_launch(&store, all, 2, &gaming, &now, 0, 1, &d) && d.width == 2816);

    // what's already right is a keep: here only the cap changes, then nothing does
    GameValues remembered = { 3840, 2160, 240, 120 }, settled = { 3840, 2160, 240, 0 };
    assert(display_policy_launch(&store, all, 2, &gaming, &remembered, 1, 1, &d));
    assert(d.width == 0 && d.height == 0 && d.refresh == 0 && d.max_fps == 240);
    assert(!display_policy_launch(&store, all, 2, &gaming, &settled, 1, 1, &d));
}

static void test_fit_window(void) {
    // 1280x800 on a 1080x1920 portrait monitor: as wide as the monitor, at the window's shape
    DisplayInfo portrait = another(&tv, 4, "44444444-4444-4444-4444-444444444444");
    portrait.w = 1080;
    portrait.h = 1920;
    int w = 1280, h = 800;
    assert(display_policy_fit_window(&portrait, &w, &h) && w == 1080 && h == 674);
    // a window that fits stays as it is
    w = 1280, h = 800;
    assert(!display_policy_fit_window(&gaming, &w, &h) && w == 1280 && h == 800);
    w = 1080, h = 674;
    assert(!display_policy_fit_window(&portrait, &w, &h) && w == 1080 && h == 674);
    // too tall: the menu bar (as tall as the 32 point camera housing) and the title bar come off the built-in's 1117
    w = 1728, h = 1117;
    assert(display_policy_fit_window(&builtin, &w, &h) && h == 1056 && w == 1634);
    // without a housing, the usual 24 point menu bar
    DisplayInfo plain = builtin;
    plain.notch = 0;
    w = 1728, h = 1117;
    assert(display_policy_fit_window(&plain, &w, &h) && h == 1064 && w == 1646);
    // nothing to go by
    w = 0, h = 0;
    assert(!display_policy_fit_window(&portrait, &w, &h));
    DisplayInfo none = portrait;
    none.w = none.h = 0;
    w = 1280, h = 800;
    assert(!display_policy_fit_window(&none, &w, &h) && w == 1280 && h == 800);
}

static void test_parse(void) {
    GameValues v;
    assert(display_policy_parse_game("game=2624x1696 refresh=120 maxfps=120", &v));
    assert(v.width == 2624 && v.height == 1696 && v.refresh == 120 && v.max_fps == 120);
    assert(display_policy_parse_game("game=1920x1080 refresh=119.88 maxfps=0", &v) && v.refresh == 120 && v.max_fps == 0);
    assert(!display_policy_parse_game("game=0x0 refresh=0 maxfps=0", &v));
    assert(!display_policy_parse_game("", &v));
}

static void test_store(void) {
    char path[] = "/tmp/test_display_policy.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    DisplayStore store = { 0 };
    RememberedDisplay *a = display_store_add(&store, &builtin);
    display_store_add(&store, &gaming);
    a->width = 2624;
    a->height = 1696;
    a->refresh = 120;
    // adding it again keeps its values and makes it the most recently used one, so it's last now
    a = display_store_add(&store, &builtin);
    assert(a == &store.displays[1] && store.count == 2 && a->width == 2624 && a->refresh == 120);
    assert(strcmp(store.displays[0].key, Gaming_UUID) == 0);
    DisplayInfo second = gaming;
    strcpy(second.key, Gaming_UUID "#3");
    display_store_add(&store, &second);
    strcpy(store.last_display, Gaming_UUID);
    strcpy(store.last_external, Gaming_UUID);
    strcpy(store.chosen, Gaming_UUID "#3");
    strcpy(store.chosen_name, "Gaming monitor (2)");
    assert(display_store_save(&store, path));

    DisplayStore loaded;
    assert(display_store_load(&loaded, path));
    assert(loaded.count == 3 && strcmp(loaded.last_display, Gaming_UUID) == 0 && strcmp(loaded.last_external, Gaming_UUID) == 0);
    assert(strcmp(loaded.chosen, Gaming_UUID "#3") == 0 && strcmp(loaded.chosen_name, "Gaming monitor (2)") == 0);
    RememberedDisplay *la = display_store_find(&loaded, Builtin_UUID), *lb = display_store_find(&loaded, Gaming_UUID);
    assert(la != NULL && lb != NULL && display_store_find(&loaded, Gaming_UUID "#3") != NULL);
    assert(strcmp(la->name, builtin.name) == 0 && la->native_w == 3456 && la->native_h == 2234 && la->max_refresh == 120);
    assert(la->width == 2624 && la->height == 1696 && la->refresh == 120 && la->vendor == 1552 && la->model == 41011);
    assert(lb->native_w == 3840 && lb->max_refresh == 240 && lb->width == 0 && lb->refresh == 0 && lb->serial == 777);

    FILE *fp = fopen(path, "w");
    fputs("// comment\nLastDisplay = abc\n\n[abc]\nName = Some TV (HDMI)\nUnknown = 1\nWindowSize = 1280x720\n", fp);
    fclose(fp);
    assert(display_store_load(&loaded, path) && loaded.count == 1 && strcmp(loaded.displays[0].name, "Some TV (HDMI)") == 0);
    assert(loaded.displays[0].width == 1280 && loaded.displays[0].height == 720 && strcmp(loaded.last_display, "abc") == 0);
    unlink(path);
    assert(!display_store_load(&loaded, path) && loaded.count == 0);
}

// 32 displays remembered: a new one makes room by forgetting the one used longest ago that isn't the last display,
// the last external or the chosen one
static void test_store_full(void) {
    DisplayStore store = { 0 };
    DisplayInfo d = tv;
    for (int i = 0; i < 32; i++) {
        snprintf(d.key, sizeof(d.key), "DISPLAY-%02d", i);
        assert(display_store_add(&store, &d) != NULL);
    }
    assert(store.count == 32);
    strcpy(store.last_display, "DISPLAY-00");
    strcpy(store.last_external, "DISPLAY-01");
    strcpy(store.chosen, "DISPLAY-02");
    strcpy(d.key, "DISPLAY-NEW");
    RememberedDisplay *added = display_store_add(&store, &d);
    assert(added != NULL && strcmp(added->key, "DISPLAY-NEW") == 0 && store.count == 32);
    assert(display_store_find(&store, "DISPLAY-03") == NULL);
    assert(display_store_find(&store, "DISPLAY-00") && display_store_find(&store, "DISPLAY-01") && display_store_find(&store, "DISPLAY-02"));
    // using 04 again moves it to the back, so 05 is forgotten next
    strcpy(d.key, "DISPLAY-04");
    display_store_add(&store, &d)->width = 1280;
    strcpy(d.key, "DISPLAY-NEWER");
    assert(display_store_add(&store, &d) != NULL && store.count == 32);
    assert(display_store_find(&store, "DISPLAY-05") == NULL && display_store_find(&store, "DISPLAY-04")->width == 1280);

    // nothing it can forget: three displays and all of them in use
    DisplayStore tiny = { 0 };
    for (int i = 0; i < 32; i++) {
        snprintf(d.key, sizeof(d.key), "SAME");
        snprintf(tiny.displays[i].key, sizeof(tiny.displays[i].key), "K%d", i % 3);
    }
    tiny.count = 32;
    strcpy(tiny.last_display, "K0");
    strcpy(tiny.last_external, "K1");
    strcpy(tiny.chosen, "K2");
    strcpy(d.key, "ANOTHER");
    assert(display_store_add(&tiny, &d) == NULL && tiny.count == 32);
}

// a broken displays.ini: whatever can be read is, nothing else, and no section's keys leak into the top level
static void test_store_corrupt(void) {
    char path[] = "/tmp/test_display_policy.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    static const char binary[] = "\xff\xfe\x00garbage\r\n";
    fwrite(binary, 1, sizeof(binary) - 1, fp);
    fputs("LastDisplay = KEEP\r\n[unterminated\r\nChosen = LEAKED\r\n[]\r\nChosen = LEAKED2\r\n", fp);
    fputs("[GOOD]\r\nName = A TV\r\nWindowSize = 1280xjunk\r\nRefreshRate = abc\r\nNative = 3840x2160\r\n", fp);
    for (int i = 0; i < 600; i++)
        fputc('x', fp);
    fputs(" = 1\n", fp);
    for (int i = 0; i < 40; i++)
        fprintf(fp, "[S%d]\nName = s%d\nChosen = LEAKED3\n", i, i);
    fclose(fp);
    DisplayStore store;
    assert(display_store_load(&store, path));
    assert(store.count == 32 && strcmp(store.displays[0].key, "GOOD") == 0 && strcmp(store.displays[0].name, "A TV") == 0);
    assert(store.displays[0].native_w == 3840 && store.displays[0].native_h == 2160 && store.displays[0].refresh == 0);
    // "1280xjunk": the width is read, the height isn't, and a size needs both
    assert(store.displays[0].height == 0);
    assert(strcmp(store.last_display, "KEEP") == 0 && store.chosen[0] == '\0');
    unlink(path);
}

// a displays.ini the user made read-only stays as it is (renaming over it would only need the folder writable)
static void test_store_read_only(void) {
    char path[] = "/tmp/test_display_policy.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    DisplayStore store = { 0 };
    display_store_add(&store, &builtin)->width = 2624;
    assert(display_store_save(&store, path) == 1);
    assert(chmod(path, 0444) == 0);
    display_store_add(&store, &gaming);
    assert(display_store_save(&store, path) == -1);
    DisplayStore loaded;
    assert(display_store_load(&loaded, path) && loaded.count == 1);
    assert(chmod(path, 0644) == 0);
    assert(display_store_save(&store, path) == 1 && display_store_load(&loaded, path) && loaded.count == 2);
    unlink(path);
    // missing: it's made
    assert(display_store_save(&store, path) == 1);
    unlink(path);
}

static void test_debounce(void) {
    // an unplug: Begin for each display, Remove, Moved/SetMain, DesktopShape, then an Add 300 ms later
    DisplayDebounce d = { 0 };
    assert(display_debounce_wait(&d, 0.0, 0.75, 5.0) == -1);
    display_debounce_poke(&d, 0.00, 1);
    display_debounce_poke(&d, 0.00, 1);
    display_debounce_poke(&d, 0.01, 0);
    display_debounce_poke(&d, 0.01, 0);
    display_debounce_poke(&d, 0.02, 0);
    assert(display_debounce_wait(&d, 0.5, 0.75, 5.0) > 0);
    display_debounce_poke(&d, 0.30, 0);
    assert(display_debounce_wait(&d, 1.0, 0.75, 5.0) > 0 && display_debounce_wait(&d, 1.05, 0.75, 5.0) == 0);
    display_debounce_clear(&d);
    assert(display_debounce_wait(&d, 2.0, 0.75, 5.0) == -1);

    // stuck in a reconfiguration (no callback after Begin): the cap still decides
    display_debounce_poke(&d, 0.0, 1);
    assert(display_debounce_wait(&d, 4.9, 0.75, 5.0) > 0 && display_debounce_wait(&d, 5.0, 0.75, 5.0) == 0);

    // events that keep coming: never later than the cap
    display_debounce_clear(&d);
    for (double t = 0; t < 6.0; t += 0.5)
        display_debounce_poke(&d, t, 0);
    assert(display_debounce_wait(&d, 5.5, 0.75, 5.0) == 0);
}

// the 50 ms timer's looks while the pointer sits on the game's display, from..to
static void look_inside(CursorEdge *edge, double from, double to) {
    for (double t = from; t < to; t += 0.05)
        assert(cursor_edge_sample(edge, t, 0) == Cursor_Stay);
}

static void test_cursor_edge(void) {
    // a single warp onto another display (what S9 does from another process): pulled back, and again later
    CursorEdge edge = { 0 };
    assert(cursor_edge_sample(&edge, 1.00, 1) == Cursor_Pull_Back);
    look_inside(&edge, 1.05, 2.0);
    assert(cursor_edge_sample(&edge, 2.00, 1) == Cursor_Pull_Back);

    // an overshoot while moving fast (0.1 s past the edge, mouse events every 8 ms): held back the whole time
    cursor_edge_reset(&edge);
    for (double t = 10.0; t < 10.1; t += 0.008)
        assert(cursor_edge_sample(&edge, t, 1) == Cursor_Pull_Back);
    look_inside(&edge, 10.15, 11.0);
    assert(!edge.released);

    // pushing on for half a second gets through, and it stays free while it's off the game's display
    cursor_edge_reset(&edge);
    CursorAction action = Cursor_Pull_Back;
    double t = 20.0;
    for (; action == Cursor_Pull_Back && t < 21.0; t += 0.008)
        action = cursor_edge_sample(&edge, t, 1);
    assert(action == Cursor_Let_Through && t > 20.45 && t < 20.6);
    assert(cursor_edge_sample(&edge, 21.5, 1) == Cursor_Let_Through);
    // back on the game's display: the edge holds again
    assert(cursor_edge_sample(&edge, 22.0, 0) == Cursor_Stay && !edge.released);
    assert(cursor_edge_sample(&edge, 22.05, 1) == Cursor_Pull_Back);

    // a second try soon after the first (a pause on the game's display in between) gets through
    cursor_edge_reset(&edge);
    assert(cursor_edge_sample(&edge, 30.00, 1) == Cursor_Pull_Back);
    look_inside(&edge, 30.05, 30.3);
    assert(cursor_edge_sample(&edge, 30.30, 1) == Cursor_Let_Through);
    assert(cursor_edge_sample(&edge, 30.35, 1) == Cursor_Let_Through);

    // a second try after 0.6 s is a new first try
    cursor_edge_reset(&edge);
    assert(cursor_edge_sample(&edge, 40.00, 1) == Cursor_Pull_Back);
    look_inside(&edge, 40.05, 40.8);
    assert(cursor_edge_sample(&edge, 40.80, 1) == Cursor_Pull_Back);

    // the game becoming active again re-arms it
    cursor_edge_reset(&edge);
    edge.released = 1;
    assert(cursor_edge_sample(&edge, 50.0, 1) == Cursor_Let_Through);
    cursor_edge_reset(&edge);
    assert(cursor_edge_sample(&edge, 50.1, 1) == Cursor_Pull_Back);
}

int main(void) {
    test_target();
    test_chosen();
    test_choices();
    test_identity();
    test_moving();
    test_same_display_changed();
    test_refresh();
    test_launch();
    test_fit_window();
    test_parse();
    test_store();
    test_store_full();
    test_store_corrupt();
    test_store_read_only();
    test_debounce();
    test_cursor_edge();
    printf("display policy tests passed\n");
    return 0;
}
