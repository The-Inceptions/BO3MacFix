// Follows display changes: which display the game uses, and its resolution and refresh rate there, see display_follow.m.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct DisplayInfo {
    uint32_t id;            // CGDirectDisplayID
    char uuid[40];          // CGDisplayCreateUUIDFromDisplayID as a string, "" if unknown
    char name[64];          // NSScreen.localizedName (or a CG fallback)
    int builtin, main;
    int x, y, w, h;         // bounds in global CG points
    int pixel_w, pixel_h;   // the current mode's pixels (the backing)
    int native_w, native_h; // the panel's native pixels
    double scale;           // backing scale factor
    int refresh;            // maximum frames per second (NSScreen.maximumFramesPerSecond, else the mode's, else 60)
    int variable_refresh;   // ProMotion or VRR
    int notch;              // camera housing height in points, 0 if none
    char key[52];           // what displays.ini knows it by: the UUID, or "<uuid>#<unit>" for a second display with that UUID
    uint32_t vendor, model, serial; // CGDisplayVendorNumber, ModelNumber and SerialNumber (0 if it has no serial)
    uint32_t unit;          // CGDisplayUnitNumber
} DisplayInfo;

typedef struct DisplayDecision {
    uint32_t display;       // the target CGDirectDisplayID (SDL display 0 once tier 1 ran)
    int width, height;      // the game resolution to use; 0 = keep r_mode (1 snaps it to an exact entry either way)
    int refresh;            // r_refreshRate to use; 0 = keep
    int max_fps;            // com_maxfps to set; -1 = keep
    int moved;              // the game's display changed, so the fullscreen window must be re-homed
    char why[64];           // for the log
} DisplayDecision;

void display_follow_init(int follow);                    // dylib init, before the game's main(); detection always, actions if follow
int display_follow_current(DisplayInfo *out);            // the game's display; any thread; 0 before the first snapshot
uint32_t display_follow_launch_target(void);             // main thread, ASL_BuildModeTable hook: the display to start on, 0 = keep SDL's order
int display_follow_launch_choice(DisplayDecision *out);  // engine thread, R_SetWndParms hook at launch: 1 = apply out, 0 = keep config.ini's values
                                                         // (out comes in holding them: width/height = r_mode, refresh = r_refreshRate, max_fps = com_maxfps)
int display_follow_launch_window(int *width, int *height); // engine thread, R_SetWndParms hook at launch, windowed: 1 = a
                                                         // smaller size of its shape in place of r_mode's, which doesn't fit
void display_follow_note_choice(int width, int height, int refresh); // engine thread: the user changed resolution/refresh in the game's options
void display_follow_expect_changes(double seconds);      // any thread: our own moves and restarts are about to cause display events
int display_follow_owns(const char *name);               // for settings.c's apply_live
int display_follow_set_option(const char *name, int value); // 1 = applied live, 0 = not (then it needs a restart)
void display_follow_save(void);                          // at quit, including ExitFix's _exit path
void display_follow_print_state(const char *why);        // one "display state: ..." line (main thread)

// GameDisplay on the settings page, any thread. The choices: 0 "Main display", 1 "Built-in", 2 "External", then each
// connected display as 100 + i with its name ("XB273K GP (2)" for a second one of that name). Returns how many.
int display_follow_game_display_choices(int *values, char labels[][64], int max);
// the value to show: 0, 1 or 2; 100 + i for the chosen display while it's connected; 3 when it isn't
int display_follow_game_display_current(void);
// what settings.ini keeps for a value the page set: 3 (the chosen display) for GameDisplay values from 100, else value
int display_follow_persisted_value(const char *name, int value);
