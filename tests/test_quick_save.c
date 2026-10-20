// Tests for quick_save_rules.c: L3 + R3 held for the quick save, and the request waiting for checkpoint.lua's timer.
#include <stdio.h>
#include "../source/quick_save_rules.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define L3 0x0040
#define R3 0x0080
#define CROSS 0x1000
#define BACK 0x0020

static void test_combo(void) {
    quick_save_combo_t combo = { 0 };
    // one at a time doesn't count
    CHECK(!quick_save_combo(&combo, L3, 10.0));
    CHECK(!quick_save_combo(&combo, L3, 12.0));
    CHECK(!quick_save_combo(&combo, R3, 13.0));
    // both: once, a second after they went down together
    CHECK(!quick_save_combo(&combo, L3 | R3, 20.0));
    CHECK(!quick_save_combo(&combo, L3 | R3, 20.5));
    CHECK(!quick_save_combo(&combo, L3 | R3, 20.99));
    CHECK(quick_save_combo(&combo, L3 | R3, 21.0));
    CHECK(!quick_save_combo(&combo, L3 | R3, 21.1));
    CHECK(!quick_save_combo(&combo, L3 | R3, 30.0)); // still the same hold
    // letting go starts over
    CHECK(!quick_save_combo(&combo, 0, 30.1));
    CHECK(!quick_save_combo(&combo, L3 | R3, 31.0));
    CHECK(quick_save_combo(&combo, L3 | R3, 32.2));
    // a third button (pressing on through the menu, or playing) breaks the hold
    quick_save_combo_t other = { 0 };
    CHECK(!quick_save_combo(&other, L3 | R3, 40.0));
    CHECK(!quick_save_combo(&other, L3 | R3 | CROSS, 40.6));
    CHECK(!quick_save_combo(&other, L3 | R3, 41.1));
    CHECK(!quick_save_combo(&other, L3 | R3, 41.5));
    CHECK(quick_save_combo(&other, L3 | R3, 42.1));
    quick_save_combo_t back = { 0 };
    CHECK(!quick_save_combo(&back, L3 | R3 | BACK, 0.0));
    CHECK(!quick_save_combo(&back, L3 | R3 | BACK, 5.0));
    // a clock at 0 still counts the hold from there
    quick_save_combo_t zero = { 0 };
    CHECK(!quick_save_combo(&zero, L3 | R3, 0.0));
    CHECK(quick_save_combo(&zero, L3 | R3, 1.01));
}

static void test_slot(void) {
    quick_save_slot_t slot = { 0 };
    CHECK(quick_save_slot_take(&slot, 5.0) == Quick_Save_None);
    quick_save_slot_put(&slot, Quick_Save_Key, 10.0);
    CHECK(quick_save_slot_take(&slot, 10.4) == Quick_Save_Key);
    CHECK(quick_save_slot_take(&slot, 10.5) == Quick_Save_None); // taken once
    // the newest request wins
    quick_save_slot_put(&slot, Quick_Save_Key, 20.0);
    quick_save_slot_put(&slot, Quick_Save_Pad, 20.1);
    CHECK(quick_save_slot_take(&slot, 20.2) == Quick_Save_Pad);
    // one nobody took for a while (no UI timer: a map loading) is dropped, not acted on later
    quick_save_slot_put(&slot, Quick_Save_Key, 30.0);
    CHECK(quick_save_slot_take(&slot, 30.0 + Quick_Save_Max_Age + 0.1) == Quick_Save_None);
    CHECK(quick_save_slot_take(&slot, 31.0) == Quick_Save_None);
}

int main(void) {
    test_combo();
    test_slot();
    if (failures == 0)
        printf("test_quick_save: all passed\n");
    return failures == 0 ? 0 : 1;
}
