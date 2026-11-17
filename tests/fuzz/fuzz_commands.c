// The console commands and short texts a mod, a config or another program gives BO3MacFix, under the fuzzer (make
// fuzz): customkeymap, SendMenuResponse and DB_FreeUnusedResources (t7o_small.c), printsubtitle (t7o_subtitle.c), the
// command line (launch_args.c), the players folder's paths (players_cfg.c), the names and addresses OfflineMode looks
// at (offline_mode.c) and the notices' queue (notice_queue.c).
//
// The input's first byte picks what, the rest is split at NULs or newlines into a command's arguments.
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "../../source/havok.h"
#include "../../source/launch_args.h"
#include "../../source/notice_queue.h"
#include "../../source/offline_mode.h"
#include "../../source/players_cfg.h"
#include "../../source/t7o_small.h"
#include "../../source/t7o_subtitle.h"
#include "../../source/t7overcharged.h"
// (their .c files are built beside it: see FUZZ_SOURCES_commands in the Makefile)

// what t7o_small.c's game side asks of the rest of BO3MacFix; the harness never reaches it
int BO3MacFix_Hook(void *address, void *replacement, void **original) {
    (void)address, (void)replacement, (void)original;
    return 0;
}
void hks_push_nil(lua_State *s) {
    (void)s;
}
void hks_push_string(lua_State *s, const char *text) {
    (void)s, (void)text;
}
const char *const *t7o_command_args(int *argc) {
    *argc = 0;
    return NULL;
}

#define Most_Args 12

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0)
        return 0;
    uint8_t which = data[0];
    char *text = malloc(size);
    memcpy(text, data + 1, size - 1);
    text[size - 1] = '\0';
    // the arguments: each its own allocation of its exact length, so a read past one's end is seen
    char *argv[Most_Args + 1];
    int argc = 0;
    for (char *part = text; argc < Most_Args;) {
        size_t length = strcspn(part, "\n");
        argv[argc] = malloc(length + 1);
        memcpy(argv[argc], part, length);
        argv[argc][length] = '\0';
        argc++;
        if (part[length] == '\0')
            break;
        part += length + 1;
    }
    argv[argc] = NULL;
    const char *const *args = (const char *const *)argv;
    switch (which % 8) {
    case 0: { // customkeymap <name> <keys> [debounce], several times into one map
        static t7o_keymap map;
        if ((which >> 3) == 0)
            memset(&map, 0, sizeof(map));
        t7o_keymap_command(&map, argc, args);
        if (map.count < 0 || map.count > T7o_Keymap_Max)
            abort();
        for (int i = 0; i < map.count; i++)
            if (memchr(map.mappings[i].name, '\0', sizeof(map.mappings[i].name)) == NULL)
                abort();
        t7o_keyset held;
        memset(&held, 0, sizeof(held));
        for (int key = -2; key < T7o_Keys + 2; key += 37)
            t7o_keymap_find(&map, &held, key);
        break;
    }
    case 1: { // printsubtitle <text> [channel] [duration] [local client] [width] [flags]
        t7o_subtitle subtitle;
        if (t7o_parse_printsubtitle(argc, args, which >> 6, &subtitle) &&
            (memchr(subtitle.text, '\0', sizeof(subtitle.text)) == NULL || subtitle.channel < 0 ||
             subtitle.channel >= T7o_Channel_Count))
            abort();
        break;
    }
    case 2: { // SendMenuResponse <menu> <response> [debounce]
        t7o_menu_response response;
        int debounce;
        t7o_menuresponse_debounce(argc, args, &debounce);
        if (t7o_parse_sendmenuresponse(argc, args, &response)) {
            char line[320];
            if (t7o_format_menu_response(line, sizeof(line), (int)which, 3, 4, response.response) &&
                memchr(line, '\0', sizeof(line)) == NULL)
                abort();
            t7o_format_menu_response(line, 8, -1, -1, -1, response.response);
        }
        bool first, second;
        t7o_free_unused_args(argc, args, &first, &second);
        int value;
        t7o_stoi(argv[0], &value);
        break;
    }
    case 3: // the command line
        launch_args_start_game(argc, argv);
        break;
    case 4: { // the players folder: what the engine asked for, the game folder
        char out[1024];
        players_cfg_in_game_folder(argv[0], argc > 1 ? argv[1] : "/Games/BO3/", out, which & 8 ? sizeof(out) : 16);
        break;
    }
    case 5: { // a name OfflineMode is asked to look up, and an address from the same bytes
        offline_name_is_local(argv[0], argc > 1 ? argv[1] : "Mac.local");
        size_t length = size - 1 < sizeof(struct sockaddr_storage) ? size - 1 : sizeof(struct sockaddr_storage);
        struct sockaddr_storage *address = malloc(length ? length : 1);
        memcpy(address, data + 1, length);
        // (the caller's length is trusted, as the system's own calls trust it: only lengths that fit the bytes)
        offline_address_is_local((const struct sockaddr *)address, (socklen_t)length);
        offline_address_is_local((const struct sockaddr *)address, (socklen_t)(length < (size_t)(which >> 2) ? length
                                                                                : (size_t)(which >> 2)));
        free(address);
        break;
    }
    case 6: { // the notices' queue: each argument a notice, then all taken out
        static notice_queue_t queue;
        memset(&queue, 0, sizeof(queue));
        for (int i = 0; i < argc; i++) {
            if (i & 1)
                notice_queue_push_lasting(&queue, argv[i], i);
            else
                notice_queue_push(&queue, argv[i], i);
        }
        char out[256];
        bool lasting;
        for (int i = 0; i < 64 && notice_queue_pop(&queue, 100.0, (double)(which & 7), out, (which & 1) ? sizeof(out) : 8,
                                                   &lasting);
             i++)
            if (memchr(out, '\0', (which & 1) ? sizeof(out) : 8) == NULL)
                abort();
        break;
    }
    case 7: { // the number reader all of T7Overcharged's commands use
        int value = 0;
        for (int i = 0; i < argc; i++)
            t7o_stoi(argv[i], &value);
        break;
    }
    }
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(text);
    return 0;
}

void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    add("\x00" "customkeymap\nloadingskip1\n27", 29);
    add("\x08" "customkeymap\nloadingskip2\n200+27\n500", 36);
    add("\x01" "printsubtitle\n^3Round 5\n1\n3000\n0\n400\n0", 39);
    add("\x02" "SendMenuResponse\nStartMenu_Main\naae_flashlight\n200", 50);
    add("\x03" "CoDBlkOps3_Exe\n+devmap\nzm_theater\n+set\nfs_game\nusermaps", 56);
    add("\x0c/Users/x/Library/Application Support/Call of Duty Black Ops III/players/AAE.cfg\n/Games/BO3/", 93);
    add("\x05" "ops-dwprod.demonware.net\nMac.local", 34);
    add("\x06" "a notice\na lasting one\nanother", 32);
}
void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    add("+", 1);
    add("\n", 1);
    add("-2147483649", 11);
    add("4294967296", 10);
    add("+devmap", 7);
    add("+map", 4);
    add(".local", 6);
    add("localhost.", 10);
    add("players/", 8);
    add(".cfg", 4);
    add("%en0", 4);
    add("::ffff:", 7);
}
