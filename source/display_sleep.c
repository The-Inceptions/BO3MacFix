// See display_sleep.h.
#include "display_sleep.h"

bool display_sleep_decide(display_sleep_t *s, int mode, bool on_battery, bool away, bool controllers_seen, uint64_t now) {
    bool applies = controllers_seen && (mode == Display_Sleep_Always || (mode == Display_Sleep_On_Battery && on_battery));
    if (!applies || !away) {
        if (s->released)
            s->retaken_at = now;
        s->away_since = 0;
        s->released = false;
        return false;
    }
    if (s->away_since == 0)
        s->away_since = now;
    if (now - s->away_since >= Display_Sleep_Delay_NS)
        s->released = true;
    return s->released;
}

bool display_sleep_activity_due(display_sleep_t *s, uint64_t now) {
    bool waking = s->retaken_at != 0 && now - s->retaken_at < Display_Sleep_Wake_NS;
    if (!s->released && !waking)
        return false;
    if (s->declared_at != 0 && now - s->declared_at < Display_Sleep_Activity_NS)
        return false;
    s->declared_at = now;
    return true;
}

static bool past(int value, int threshold) {
    return value > threshold || value < -threshold;
}

bool display_sleep_is_input(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t left_x,
                            int16_t left_y, int16_t right_x, int16_t right_y) {
    return buttons != 0 || left_trigger > 30 || right_trigger > 30 || past(left_x, 8000) || past(left_y, 8000) ||
           past(right_x, 8000) || past(right_y, 8000);
}
