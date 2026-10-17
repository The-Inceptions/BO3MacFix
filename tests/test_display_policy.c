// Tests for display_policy.c: which display the game uses, the resolution and refresh rate it gets there,
// displays.ini, and the debounce. No game or display needed.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../source/display_policy.h"

static const DisplayInfo builtin = { 1, "37D8832A-2D66-02CA-B9F7-8F30A301B230", "Built-in Retina Display", 1, 1,
                                     0, 0, 1728, 1117, 3456, 2234, 3456, 2234, 2.0, 120, 1, 32 };
static const DisplayInfo gaming = { 2, "A5B77482-F6CF-4007-8E6B-3044BFC95B55", "Gaming monitor", 0, 0,
                                    1728, 0, 1920, 1080, 3840, 2160, 3840, 2160, 2.0, 240, 1, 0 };
static const DisplayInfo tv = { 3, "CB214AE0-3710-4F1F-8869-3AF9C23E6F65", "TV", 0, 0,
                                -1920, 0, 1920, 1080, 1920, 1080, 1920, 1080, 1.0, 60, 0, 0 };

static RememberedDisplay shape(const DisplayInfo *display) {
    RememberedDisplay out;
    display_policy_shape(display, &out);
    return out;
}

static void test_target(void) {
    DisplayInfo all[] = { builtin, gaming, tv };
    assert(display_policy_target(all, 3, 0, "") == 0);                   // Main display
    assert(display_policy_target(all, 3, 1, "") == 0);                   // Built-in
    assert(display_policy_target(all, 3, 2, "") == 1);                   // External: the fastest one
    assert(display_policy_target(all, 3, 2, tv.uuid) == 2);              // External: the last one used, when it's there
    assert(display_policy_target(all, 3, 2, builtin.uuid) == 1);         // a built-in is never "the last external"

    // the gaming monitor is the main display (lid closed, or set as main)
    DisplayInfo desk[] = { gaming, tv };
    desk[0].main = 1;
    assert(display_policy_target(desk, 2, 0, "") == 0);
    assert(display_policy_target(desk, 2, 1, "") == 0);                  // no built-in: the main display
    assert(display_policy_target(desk, 2, 2, tv.uuid) == 1);

    DisplayInfo laptop[] = { builtin };
    assert(display_policy_target(laptop, 1, 2, gaming.uuid) == 0);       // no external: the built-in
    assert(display_policy_target(NULL, 0, 0, "") == -1);

    // the same refresh rate: the one with more pixels
    DisplayInfo two[] = { builtin, tv, tv };
    two[2].id = 4;
    two[2].native_w = 3840;
    two[2].native_h = 2160;
    assert(display_policy_target(two, 3, 2, "") == 2);
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
    strcpy(store.last_display, builtin.uuid);
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
    RememberedDisplay *a = display_store_add(&store, &builtin), *b = display_store_add(&store, &gaming);
    a->width = 2624;
    a->height = 1696;
    a->refresh = 120;
    assert(display_store_add(&store, &builtin) == a && store.count == 2);
    strcpy(store.last_display, gaming.uuid);
    strcpy(store.last_external, gaming.uuid);
    assert(display_store_save(&store, path));

    DisplayStore loaded;
    assert(display_store_load(&loaded, path));
    assert(loaded.count == 2 && strcmp(loaded.last_display, gaming.uuid) == 0 && strcmp(loaded.last_external, gaming.uuid) == 0);
    RememberedDisplay *la = display_store_find(&loaded, builtin.uuid), *lb = display_store_find(&loaded, gaming.uuid);
    assert(la != NULL && lb != NULL);
    assert(strcmp(la->name, builtin.name) == 0 && la->native_w == 3456 && la->native_h == 2234 && la->max_refresh == 120);
    assert(la->width == 2624 && la->height == 1696 && la->refresh == 120);
    assert(lb->native_w == 3840 && lb->max_refresh == 240 && lb->width == 0 && lb->refresh == 0);
    (void)b;

    FILE *fp = fopen(path, "w");
    fputs("// comment\nLastDisplay = abc\n\n[abc]\nName = Some TV (HDMI)\nUnknown = 1\nWindowSize = 1280x720\n", fp);
    fclose(fp);
    assert(display_store_load(&loaded, path) && loaded.count == 1 && strcmp(loaded.displays[0].name, "Some TV (HDMI)") == 0);
    assert(loaded.displays[0].width == 1280 && loaded.displays[0].height == 720 && strcmp(loaded.last_display, "abc") == 0);
    unlink(path);
    assert(!display_store_load(&loaded, path) && loaded.count == 0);
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

int main(void) {
    test_target();
    test_moving();
    test_same_display_changed();
    test_refresh();
    test_launch();
    test_parse();
    test_store();
    test_debounce();
    printf("display policy tests passed\n");
    return 0;
}
