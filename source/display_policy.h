// The decisions behind following display changes, in plain C so the tests can run them (display_follow.m is the macOS
// side): which display the game uses, what resolution and refresh rate it gets there, displays.ini, telling displays
// apart, the debounce, and ConfineCursor's soft edge.
#pragma once
#include "display_follow.h"

#define Display_Max 16

// refresh rates outside this range aren't believed (a virtual display reported 2500 Hz)
#define Display_Refresh_Low 24
#define Display_Refresh_High 500
// the most the game's RefreshRate (r_refreshRate) and MaxFPS (com_maxfps) are set to
#define Game_Refresh_Max 240
#define Game_MaxFPS_Max 1000

// GameDisplay values: the rules, then (on the settings page only) 100 + the index of a connected display to choose
#define Game_Display_Main 0
#define Game_Display_Builtin 1
#define Game_Display_External 2
#define Game_Display_Chosen 3
#define Game_Display_Pick 100

// what the game used on a display, kept in displays.ini by the display's key (DisplayInfo.key)
typedef struct RememberedDisplay {
    char key[52];
    char name[64];
    uint32_t vendor, model, serial;
    int native_w, native_h; // the panel's native pixels, for scaling a resolution to another display
    int max_refresh;        // the display's maximum refresh rate
    int width, height;      // the game's WindowSize there, 0 if not remembered
    int refresh;            // the game's RefreshRate there, 0 if not remembered
} RememberedDisplay;

typedef struct DisplayStore {
    RememberedDisplay displays[32];
    int count;
    char last_display[52];  // the display the game was last on
    char last_external[52]; // the last external display the game used
    char chosen[52];        // GameDisplay 3's display
    char chosen_name[64];
} DisplayStore;

// the game's r_mode, r_refreshRate and com_maxfps
typedef struct GameValues {
    int width, height, refresh, max_fps;
} GameValues;

int display_policy_find(const DisplayInfo *displays, int count, uint32_t id);
int display_policy_find_key(const DisplayInfo *displays, int count, const char *key);

// keys the displays by UUID, and a second display with the same UUID as "<uuid>#<unit number>"
void display_policy_keys(DisplayInfo *displays, int count);

// the connected display that's the one displays.ini knows as `key`: that key, else (on another port, with another UUID)
// the same vendor, model and serial, else the only display with its vendor, model, name and native size. -1 if none.
int display_policy_find_remembered(const DisplayInfo *displays, int count, const DisplayStore *store, const char *key);

// GameDisplay: 0 the main display, 1 the built-in while it's on, 2 an external display (the last one the game used,
// else the one with the highest refresh rate, then the most pixels), 3 the chosen display while it's connected, else
// as 2. Returns an index into displays, -1 if there are none.
int display_policy_target(const DisplayInfo *displays, int count, int game_display, const DisplayStore *store);

// the settings page's GameDisplay choices: the three rules, then each display as Game_Display_Pick + its index, named
// (a second "XB273K GP" is "XB273K GP (2)"). Returns how many.
int display_policy_choices(const DisplayInfo *displays, int count, int *values, char labels[][64], int max);
// the value the page shows for the rule: 0 to 2, Game_Display_Pick + i for the chosen display i, or 3 if it's not connected
int display_policy_current(int game_display, const DisplayInfo *displays, int count, const DisplayStore *store);
// what settings.ini keeps: GameDisplay's picks (Game_Display_Pick + i) are stored as 3, the chosen display
int display_policy_persisted(const char *name, int value);

// a display's refresh rate: NSScreen's maximum frames per second (0 if unknown), else its mode's, else 60, each only if
// it's within Display_Refresh_Low..High
int display_policy_refresh(int screen_fps, int mode_refresh);

// whether something the game cares about changed on a display: its bounds, pixels or scale, or its refresh rate (to
// one the game could use)
int display_policy_changed(const DisplayInfo *before, const DisplayInfo *after);

// a display's shape (key, name, identity, native pixels, maximum refresh rate) without any of the game's values
void display_policy_shape(const DisplayInfo *display, RememberedDisplay *out);

// what to set when the game goes from `from`, where it runs at `now`, to `to`. A different display gets its remembered
// values (to_memory, may be NULL), else as many pixels as now (the same GPU cost) at its shape, up to its native
// resolution; the same display that changed only follows with the refresh rate. With match_refresh, RefreshRate is the
// display's own, and a MaxFPS that equalled the old display's refresh rate becomes the new display's.
void display_policy_decide(const RememberedDisplay *from, const GameValues *now, const DisplayInfo *to,
                           const RememberedDisplay *to_memory, int match_refresh, DisplayDecision *out);

// at launch, on `to`: when config.ini's values (now) belong to another display (the one the game was last on, or else
// the display whose shape they fit), what to use instead, with 0 (or -1 for max_fps) for what stays. Returns 1 if out
// changes something.
int display_policy_launch(const DisplayStore *store, const DisplayInfo *displays, int count, const DisplayInfo *to,
                          const GameValues *now, int use_memory, int match_refresh, DisplayDecision *out);

// reads engine_display_describe's "game=WxH refresh=N maxfps=N", returns 1 if the size was there
int display_policy_parse_game(const char *text, GameValues *out);

RememberedDisplay *display_store_find(DisplayStore *store, const char *key);
// finds or adds the display and updates its shape; NULL if the store is full
RememberedDisplay *display_store_add(DisplayStore *store, const DisplayInfo *display);
// displays[i] isn't in the store under its key: if it's a remembered display that came back with another UUID (the
// same vendor, model and serial, else the only remembered display of its kind that isn't connected), add it with that
// memory, point the last-used and chosen display at it, and return 1 with the old key in `from`
int display_store_adopt(DisplayStore *store, const DisplayInfo *displays, int count, int i, char *from, size_t n);
int display_store_load(DisplayStore *store, const char *path);
int display_store_save(const DisplayStore *store, const char *path); // written atomically, 1 on success

// trailing-edge debounce of display events: a decision is due `settle` seconds after the last event, but not while a
// reconfiguration is in progress (a CG "begin" with nothing after it yet), and never later than `cap` after the first
typedef struct DisplayDebounce {
    double first, last;
    int pending, in_config;
} DisplayDebounce;

void display_debounce_poke(DisplayDebounce *d, double now, int begin);
double display_debounce_wait(const DisplayDebounce *d, double now, double settle, double cap); // seconds, 0 = due, -1 = nothing pending
void display_debounce_clear(DisplayDebounce *d);

// ConfineCursor's soft edge, fed every look at the pointer (a mouse event or the 50 ms timer): the first time it's off
// the game's display it's pulled back. An attempt is the pointer off the display, with the looks less than
// Edge_Attempt_Gap apart; pushing for Edge_Push_Total within one attempt, or starting a second attempt within
// Edge_Again_Window of the last one, lets it through, and it stays free until it's back on the game's display (or
// cursor_edge_reset, when the game becomes active again).
#define Edge_Attempt_Gap 0.15
#define Edge_Again_Window 0.6
#define Edge_Push_Total 0.5
#define Edge_Step_Max 0.1

typedef enum { Cursor_Stay, Cursor_Pull_Back, Cursor_Let_Through } CursorAction;

typedef struct CursorEdge {
    int released, in_attempt;
    double last_outside; // the last look with the pointer off the game's display
    double attempt_end;  // when the previous attempt ended, 0 if there wasn't one
    double pushed;       // seconds of pushing in this attempt
} CursorEdge;

CursorAction cursor_edge_sample(CursorEdge *edge, double now, int outside);
void cursor_edge_reset(CursorEdge *edge);
