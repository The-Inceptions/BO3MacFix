// See quick_save_rules.h.
#include "quick_save_rules.h"

bool quick_save_combo(quick_save_combo_t *combo, uint16_t buttons, double now) {
    if (buttons != Quick_Save_Buttons) {
        combo->since = 0;
        combo->fired = false;
        return false;
    }
    if (combo->since <= 0) {
        // a clock at 0 still counts as down
        combo->since = now > 0 ? now : 1e-9;
        return false;
    }
    if (combo->fired || now - combo->since < Quick_Save_Hold)
        return false;
    combo->fired = true;
    return true;
}

void quick_save_slot_put(quick_save_slot_t *slot, quick_save_source_t source, double now) {
    slot->source = source;
    slot->time = now;
}

quick_save_source_t quick_save_slot_take(quick_save_slot_t *slot, double now) {
    quick_save_source_t source = slot->source;
    double age = now - slot->time;
    slot->source = Quick_Save_None;
    return source != Quick_Save_None && age >= 0 && age <= Quick_Save_Max_Age ? source : Quick_Save_None;
}
