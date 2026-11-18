// IdleDisplaySleep: lets the Mac turn its display off while nobody can be playing, as it does for any other app.
//
// The game's SDL disables the screen saver when it starts: a PreventUserIdleDisplaySleep assertion ("Call of Duty:
// Black Ops III using SDL_DisableScreenSaver") that it holds until it quits. A controller's input isn't user activity to
// macOS (only keyboards, mice and trackpads are), so without it the display would go dark in the middle of a match
// played with a controller. But it also holds while the game is paused, in the menus or behind another app: a Mac left
// like that keeps its backlight on, and the game drawing, until someone comes back or the battery is flat, whatever
// "Turn display off when inactive" says (the user's Mac: 2 minutes on battery).
//
// With IdleDisplaySleep the library lets go of that assertion once the player has been away from play (the pause menu
// open, the menus, the background) for Display_Sleep_Delay_NS, and takes it again as soon as play goes on (the pause
// menu or the menus close, the game comes to the front in a match, a map starts loading). While it is let go, macOS's
// own idle time decides, and a controller's input is declared as user activity at most every Display_Sleep_Activity_NS,
// so menus navigated with a controller stay lit and a button wakes a display that went off. 1 does that on battery
// only, 2 always.
//
// This file is the decisions as plain C (tests/test_display_sleep.c); display_sleep_mac.c finds the assertion (it
// interposes IOPMAssertionCreateWithDescription, as qol.c does other system calls), lets it go, takes it again and
// declares activity, all on the main thread where SDL made it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define Display_Sleep_Delay_NS (5 * 1000000000ULL)    // away from play this long before the assertion goes
#define Display_Sleep_Activity_NS (10 * 1000000000ULL) // a controller's activity is declared at most this often
// after the assertion is taken back, a controller's input is still declared for this long: taking it back doesn't
// light a display that went off (Apple's IOPMLib.h), and play can go on without this player's input (a co-op host
// lifting the pause, a lobby's host starting the match), so the glue also declares once at the moment it's taken back
#define Display_Sleep_Wake_NS (30 * 1000000000ULL)

enum { Display_Sleep_Off = 0, Display_Sleep_On_Battery = 1, Display_Sleep_Always = 2 };

typedef struct {
    uint64_t away_since;  // when the player went away from play, 0 while playing
    bool released;        // the decision: the game's assertion is let go
    uint64_t declared_at; // when a controller's input was last declared as activity, 0 never
    uint64_t retaken_at;  // when the assertion was last taken back after a release, 0 never
} display_sleep_t;

// whether the game's assertion should be let go at now (now > 0): mode (Display_Sleep_*), on battery, away from play
// (frame_limit_away_from_play), and whether a controller's input can be seen (ControllerBridge: without it a
// controller couldn't keep the display on, nor wake it, so nothing is let go). Let go once away has lasted
// Display_Sleep_Delay_NS with the mode applying; back the moment it doesn't (play goes on, plugged in with mode 1, the
// setting off), and the moment of that is remembered (retaken_at).
bool display_sleep_decide(display_sleep_t *s, int mode, bool on_battery, bool away, bool controllers_seen, uint64_t now);

// whether a controller's input at now is declared as user activity: while the assertion is let go and for
// Display_Sleep_Wake_NS after it was taken back, at most every Display_Sleep_Activity_NS
bool display_sleep_activity_due(display_sleep_t *s, uint64_t now);

// whether a controller's state is input a player gives: a button, a trigger past 30 of 255, a stick past 8000 of
// 32767 (a resting stick reads a few hundred)
bool display_sleep_is_input(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t left_x,
                            int16_t left_y, int16_t right_x, int16_t right_y);
