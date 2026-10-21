// The arguments of T7Overcharged's printsubtitle console command, which All-around Enhancement's HUD runs for each
// game message (see t7overcharged.c). Kept apart from the game so they can be tested on their own.
#pragma once
#include <stdbool.h>
#include <stddef.h>

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

// The arguments of the SendMenuResponse console command, which All-around Enhancement's key binds run
// (gamedata/configs/common/aae_keybind.cfg, the pause menu's "Enhanced Controls"):
//     bind Y "SendMenuResponse StartMenu_Main aae_flashlight 200"
// argv[1] is the menu, argv[2] the response its GSC waits for; anything after them (the 200, which the GSC never
// gets) is left alone. False without both, or when either has a character that could end the console line the
// response goes out on (a control character, ';' or '"').
typedef struct {
    const char *menu;     // argv[1]
    const char *response; // argv[2]
} t7o_menu_response;
bool t7o_parse_sendmenuresponse(int argc, const char *const *argv, t7o_menu_response *out);

// The console line Engine.SendMenuResponse queues for a response, the same two ways the game writes it:
// "cmd mrp <server id> <menu index> <response index>" for a response the server has an index for (response_index
// above 0), "cmd mr <server id> <menu index> <response>" for any other. False when it doesn't fit in size.
bool t7o_format_menu_response(char *buffer, size_t size, int server_id, int menu_index, int response_index,
                              const char *response);
