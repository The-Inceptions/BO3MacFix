// The plain C half of the quick save (quick_save.m): when L3 + R3 on a controller count as held, and the request that
// waits for checkpoint.lua's timer. No game or macOS calls, so tests/test_quick_save.c tests it as is.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define Quick_Save_Hold 1.0     // seconds L3 + R3 are held for a quick save
#define Quick_Save_Max_Age 3.0  // seconds a request waits for checkpoint.lua's timer, which looks twice a second
#define Quick_Save_Buttons 0x00C0 // XInput's left and right thumb (stick click) buttons

typedef enum { Quick_Save_None, Quick_Save_Key, Quick_Save_Pad } quick_save_source_t;

// one controller's L3 + R3: held together, with no other button, for Quick_Save_Hold seconds
typedef struct {
    double since; // when they went down together (with nothing else), 0 while they aren't
    bool fired;   // this hold has asked already; letting go starts over
} quick_save_combo_t;

// the controller's XInput buttons now: true once a hold, as it reaches Quick_Save_Hold
bool quick_save_combo(quick_save_combo_t *combo, uint16_t buttons, double now);

// the latest request, until it's taken or too old
typedef struct {
    quick_save_source_t source;
    double time;
} quick_save_slot_t;

void quick_save_slot_put(quick_save_slot_t *slot, quick_save_source_t source, double now);
// the request, at most Quick_Save_Max_Age seconds old, or Quick_Save_None; taking it empties the slot
quick_save_source_t quick_save_slot_take(quick_save_slot_t *slot, double now);
