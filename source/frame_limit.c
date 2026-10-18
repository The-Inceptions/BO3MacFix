// See frame_limit.h.
#include "frame_limit.h"

int frame_limit_fps(int pacing_fps, int battery_fps, bool on_battery, int pause_menu_fps, bool pause_menu_open) {
    int limits[] = { pacing_fps, on_battery ? battery_fps : 0, pause_menu_open ? pause_menu_fps : 0 };
    int fps = 0;
    for (int i = 0; i < (int)(sizeof(limits) / sizeof(limits[0])); i++)
        if (limits[i] > 0 && (fps == 0 || limits[i] < fps))
            fps = limits[i];
    return fps;
}

void frame_limit_set_pause_menu(frame_limits_t *limits, bool open, uint64_t now) {
    atomic_store(&limits->pause_menu_until, open ? now + Pause_Menu_Timeout_NS : 0);
}

int frame_limit_for_frame(frame_limits_t *limits, uint64_t now, int *log) {
    int pause_menu_fps = atomic_load(&limits->pause_menu_fps);
    uint64_t until = atomic_load(&limits->pause_menu_until);
    bool open = until != 0 && now < until;
    bool limiting = open && pause_menu_fps > 0;
    *log = 0;
    if (limiting != limits->pause_menu_logged) {
        limits->pause_menu_logged = limiting;
        // stopping with the menu still open is PauseMenuFPS turned off
        *log = limiting ? 1 : open ? 0 : -1;
    }
    return frame_limit_fps(atomic_load(&limits->pacing_fps), atomic_load(&limits->battery_fps),
                           atomic_load(&limits->on_battery), pause_menu_fps, open);
}
