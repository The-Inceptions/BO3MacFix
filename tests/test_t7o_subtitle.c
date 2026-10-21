// t7o_subtitle.c: printsubtitle's arguments, as All-around Enhancement's HUD runs it for each game message
#include <stdio.h>
#include <string.h>
#include "../source/t7o_subtitle.h"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define ARGC(argv) ((int)(sizeof(argv) / sizeof((argv)[0])))

int main(void) {
    t7o_subtitle m;

    // SendMenuResponse, as AAE's key binds run it: bind Y "SendMenuResponse StartMenu_Main aae_flashlight 200"
    t7o_menu_response r;
    const char *flash[] = { "SendMenuResponse", "StartMenu_Main", "aae_flashlight", "200" };
    CHECK(t7o_parse_sendmenuresponse(ARGC(flash), flash, &r));
    CHECK(strcmp(r.menu, "StartMenu_Main") == 0 && strcmp(r.response, "aae_flashlight") == 0);
    const char *two[] = { "SendMenuResponse", "StartMenu_Main", "aae_hud" };
    CHECK(t7o_parse_sendmenuresponse(ARGC(two), two, &r) && strcmp(r.response, "aae_hud") == 0);
    const char *menu_only[] = { "SendMenuResponse", "StartMenu_Main" };
    CHECK(!t7o_parse_sendmenuresponse(ARGC(menu_only), menu_only, &r));
    const char *empty_response[] = { "SendMenuResponse", "StartMenu_Main", "" };
    CHECK(!t7o_parse_sendmenuresponse(ARGC(empty_response), empty_response, &r));
    const char *null_menu[] = { "SendMenuResponse", NULL, "aae_hud" };
    CHECK(!t7o_parse_sendmenuresponse(ARGC(null_menu), null_menu, &r));
    // nothing that could end the console line the response goes out on
    const char *chained[] = { "SendMenuResponse", "StartMenu_Main", "aae_hud;quit" };
    CHECK(!t7o_parse_sendmenuresponse(ARGC(chained), chained, &r));
    const char *newline[] = { "SendMenuResponse", "StartMenu_Main\nquit", "aae_hud" };
    CHECK(!t7o_parse_sendmenuresponse(ARGC(newline), newline, &r));
    const char *quoted[] = { "SendMenuResponse", "StartMenu_Main", "a\"b" };
    CHECK(!t7o_parse_sendmenuresponse(ARGC(quoted), quoted, &r));
    CHECK(!t7o_parse_sendmenuresponse(ARGC(flash), NULL, &r));
    CHECK(!t7o_parse_sendmenuresponse(ARGC(flash), flash, NULL));

    // the line queued: by index when the server has one for the response, by name otherwise
    char line[64];
    CHECK(t7o_format_menu_response(line, sizeof(line), 12, 3, 7, "aae_flashlight"));
    CHECK(strcmp(line, "cmd mrp 12 3 7\n") == 0);
    CHECK(t7o_format_menu_response(line, sizeof(line), 12, 3, 0, "aae_flashlight"));
    CHECK(strcmp(line, "cmd mr 12 3 aae_flashlight\n") == 0);
    CHECK(!t7o_format_menu_response(line, sizeof(line), 12, 3, 0, "x;quit") && line[0] == '\0');
    CHECK(!t7o_format_menu_response(line, 16, 12, 3, 0, "aae_flashlight") && line[0] == '\0');
    CHECK(!t7o_format_menu_response(NULL, 0, 12, 3, 7, "aae_flashlight"));

    // AAE's HUD: printsubtitle "<message>" 1 5000 <controller> 730 0 (the console takes the quotes off)
    const char *bal[] = { "printsubtitle", "Current Balance: ^3337000", "1", "5000", "0", "730", "0" };
    CHECK(t7o_parse_printsubtitle(ARGC(bal), bal, 1, &m));
    CHECK(strcmp(m.text, "Current Balance: ^3337000\n") == 0);
    CHECK(m.channel == T7o_Channel_GameNotify && m.duration == 5000 && m.local_client == 0 && m.pixel_width == 730 &&
          m.flags == 0);

    // its debug coordinates: printsubtitle Coordinate:<x,y,z> 1 20000 0 730 0
    const char *coords[] = { "printsubtitle", "Coordinate:1,2,3", "1", "20000", "0", "730", "0" };
    CHECK(t7o_parse_printsubtitle(ARGC(coords), coords, 1, &m));
    CHECK(strcmp(m.text, "Coordinate:1,2,3\n") == 0 && m.duration == 20000);

    // a message that already ends a line keeps just the one line feed
    const char *ended[] = { "printsubtitle", "done\n" };
    CHECK(t7o_parse_printsubtitle(ARGC(ended), ended, 1, &m));
    CHECK(strcmp(m.text, "done\n") == 0);

    // only a message: the defaults (gamenotify, the window's own time and width)
    const char *bare[] = { "printsubtitle", "hello" };
    CHECK(t7o_parse_printsubtitle(ARGC(bare), bare, 1, &m));
    CHECK(m.channel == T7o_Channel_GameNotify && m.duration == 0 && m.local_client == 0 && m.pixel_width == 0 &&
          m.flags == 0);

    // nothing to print
    const char *none[] = { "printsubtitle" };
    CHECK(!t7o_parse_printsubtitle(ARGC(none), none, 1, &m));
    const char *empty[] = { "printsubtitle", "", "1" };
    CHECK(!t7o_parse_printsubtitle(ARGC(empty), empty, 1, &m));
    const char *null_text[] = { "printsubtitle", NULL };
    CHECK(!t7o_parse_printsubtitle(ARGC(null_text), null_text, 1, &m));
    CHECK(!t7o_parse_printsubtitle(ARGC(bal), NULL, 1, &m));
    CHECK(!t7o_parse_printsubtitle(ARGC(bal), bal, 1, NULL));

    // numbers that aren't whole numbers or are out of range keep their defaults
    const char *bad[] = { "printsubtitle", "x", "12", "-5", "3", "73O", "-1" };
    CHECK(t7o_parse_printsubtitle(ARGC(bad), bad, 2, &m));
    CHECK(m.channel == T7o_Channel_GameNotify); // channels are 0 to 9
    CHECK(m.duration == 0 && m.pixel_width == 0 && m.flags == 0);
    CHECK(m.local_client == 0); // only clients 0 and 1 exist
    const char *big[] = { "printsubtitle", "x", "9", "99999999999", "1", "", "2" };
    CHECK(t7o_parse_printsubtitle(ARGC(big), big, 2, &m));
    CHECK(m.channel == 9 && m.duration == 0 && m.local_client == 1 && m.pixel_width == 0 && m.flags == 2);
    // no local clients known (the count unreadable or 0): client 0
    const char *client[] = { "printsubtitle", "x", "1", "5000", "3" };
    CHECK(t7o_parse_printsubtitle(ARGC(client), client, 0, &m));
    CHECK(m.local_client == 0);

    // a message too long for the game is cut, still ending in a line feed
    static char long_text[3000];
    memset(long_text, 'a', sizeof(long_text) - 1);
    const char *longer[] = { "printsubtitle", long_text, "1" };
    CHECK(t7o_parse_printsubtitle(ARGC(longer), longer, 1, &m));
    size_t length = strlen(m.text);
    CHECK(length == sizeof(m.text) - 1 && m.text[length - 1] == '\n' && m.text[length - 2] == 'a');

    if (failures == 0)
        printf("test_t7o_subtitle: all passed\n");
    return failures == 0 ? 0 : 1;
}
