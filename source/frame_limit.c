// See frame_limit.h.
#include "frame_limit.h"

int frame_limit_lowest(const frame_limit_t *limits, int count) {
    int fps = 0;
    for (int i = 0; i < count; i++)
        if (limits[i].applies && limits[i].fps > 0 && (fps == 0 || limits[i].fps < fps))
            fps = limits[i].fps;
    return fps;
}

void frame_limit_set_pause_menu(frame_limits_t *limits, bool open, uint64_t now) {
    atomic_store(&limits->pause_menu_until, open ? now + Pause_Menu_Timeout_NS : 0);
}

void frame_limit_set_menus(frame_limits_t *limits, bool in_menus, uint64_t now) {
    atomic_store(&limits->menus_until, in_menus ? now + Pause_Menu_Timeout_NS : 0);
    atomic_store(&limits->ui_until, now + Pause_Menu_Timeout_NS);
}

void frame_limit_ui_gone(frame_limits_t *limits) {
    atomic_store(&limits->menus_until, 0);
    atomic_store(&limits->ui_until, 0);
}

// whether a time limited situation (the pause menu, the menus) lasts at now
static bool lasts(_Atomic uint64_t *until, uint64_t now) {
    uint64_t t = atomic_load(until);
    return t != 0 && now < t;
}

int frame_limit_for_frame(frame_limits_t *limits, uint64_t now, int *started, int *stopped) {
    frame_limit_t all[] = {
        { atomic_load(&limits->pacing_fps), true },
        { atomic_load(&limits->battery_fps), atomic_load(&limits->on_battery) },
        { atomic_load(&limits->pause_menu_fps), lasts(&limits->pause_menu_until, now) },
        { atomic_load(&limits->menu_fps), lasts(&limits->menus_until, now) },
        { atomic_load(&limits->background_fps), atomic_load(&limits->in_background) && lasts(&limits->ui_until, now) },
    };
    // the situational ones, in Limit_* bit order, from all[2]
    int situations = 0, limiting = 0;
    for (int i = 0; i < 3; i++) {
        if (all[2 + i].applies)
            situations |= 1 << i;
        if (all[2 + i].applies && all[2 + i].fps > 0)
            limiting |= 1 << i;
    }
    int changed = limiting ^ limits->logged;
    limits->logged = limiting;
    *started = changed & limiting;
    // stopping while the situation lasts is the setting turned off
    *stopped = changed & ~limiting & ~situations;
    return frame_limit_lowest(all, (int)(sizeof(all) / sizeof(all[0])));
}
