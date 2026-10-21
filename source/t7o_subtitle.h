// The arguments of T7Overcharged's printsubtitle console command, which All-around Enhancement's HUD runs for each
// game message (see t7overcharged.c). Kept apart from the game so they can be tested on their own.
#pragma once
#include <stdbool.h>

#define T7o_Channel_GameNotify 1 // the console channel game message window 0 shows (its filter is "gamenotify obituary")
#define T7o_Channel_Count 10     // the game's own channels, 0 (dontfilter) to 9 (info)
#define T7o_Subtitle_Max_Text 1024

typedef struct {
    char text[T7o_Subtitle_Max_Text]; // ends in a line feed, so the game takes it as a whole line
    int channel;
    int duration;    // milliseconds, 0 for the window's own time
    int local_client;
    int pixel_width; // 0 for the window's own width
    int flags;
} t7o_subtitle;

// printsubtitle <text> [channel] [duration] [local client] [pixel width] [flags], as AAE runs it:
//     printsubtitle "<message>" 1 5000 <controller> 730 0
// argv[0] is the command's name. False without a text to print. A number that's missing, isn't a whole number or is
// out of range keeps its default: channel 1 (gamenotify), local client 0 (it has to be below local_clients), 0 for
// the rest. A text too long for the game is cut.
bool t7o_parse_printsubtitle(int argc, const char *const *argv, int local_clients, t7o_subtitle *out);
