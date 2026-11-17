// The player's own text files under the fuzzer (make fuzz): settings.ini (settings_ini.c), checkpoint.ini and the
// mod's custom mutations .cfg (checkpoint.c), the loadout's texts in checkpoint.ini (zm_loadout.c) and displays.ini
// (display_policy.c). A file damaged by a full disk, a crash while it was written, an editor or a download: none of it
// may make the readers read or write outside their buffers.
//
// The input's first byte picks the file, the rest is its text; the files are written to the fuzzer's out folder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../source/settings_ini.c"
#include "../../source/display_policy.c"

#include "../../source/checkpoint.h"
#include "../../source/zm_loadout.h"
// (checkpoint.c and zm_loadout.c are built beside it: see FUZZ_SOURCES_text_files in the Makefile)

static char path[256], players[256];

static void write_file(const char *to, const uint8_t *data, size_t size) {
    FILE *file = fopen(to, "wb");
    if (file == NULL)
        abort();
    fwrite(data, 1, size, file);
    fclose(file);
}

static void must_end(const char *text, size_t size) {
    if (memchr(text, '\0', size) == NULL)
        abort(); // a reader left its out text without its end
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (path[0] == '\0') {
        snprintf(path, sizeof(path), "tests/fuzz/out/text_file.%d", (int)getpid());
        snprintf(players, sizeof(players), "tests/fuzz/out/players.%d/", (int)getpid());
        mkdir(players, 0755);
    }
    if (size == 0)
        return 0;
    uint8_t which = data[0];
    data++, size--;
    char *text = malloc(size + 1);
    memcpy(text, data, size);
    text[size] = '\0';
    write_file(path, data, size);
    switch (which % 6) {
    case 0: { // settings.ini
        settings_ini_read(path, "JobThreads", 4, 10);
        settings_ini_read(path, "BatteryFPS", 0, 16);
        char out[64];
        memset(out, 'x', sizeof(out));
        if (settings_ini_read_string(path, "OfflineMode", out, sizeof(out)))
            must_end(out, sizeof(out));
        int number;
        settings_ini_number(text, which & 1 ? 16 : 10, &number);
        settings_ini_match(text, "PrePatchedExe");
        settings_ini_write(path, "DisplayFollow", "1");
        settings_ini_write(path, "", "");
        break;
    }
    case 1: { // checkpoint.ini
        checkpoint_paths_t paths = { path, players };
        static char out[Checkpoint_Read_Size];
        memset(out, 'x', sizeof(out));
        checkpoint_read(&paths, out, sizeof(out));
        must_end(out, sizeof(out));
        checkpoint_resume_state(&paths);
        checkpoint_points_later(&paths);
        char commands[2048] = "";
        checkpoint_apply(&paths, which & 1 ? "_Default" : "", commands, sizeof(commands));
        must_end(commands, sizeof(commands));
        char when[64];
        checkpoint_saved_when(text, 1700000000, when, sizeof(when));
        break;
    }
    case 2: { // the mod's custom mutations: "modvar <dvar> <value>" lines
        char value[64], changed[8192];
        memset(value, 'x', sizeof(value));
        if (aae_cfg_value(text, "tfoption_start_round", value, sizeof(value)))
            must_end(value, sizeof(value));
        if (aae_cfg_set(text, "tfoption_start_round", which & 1 ? "20" : NULL, changed, sizeof(changed)))
            must_end(changed, sizeof(changed));
        aae_cfg_set(text, "tfoption_perkaholic", "a value with spaces", changed, 16);
        break;
    }
    case 3: { // the loadout's texts, split at the first two newlines
        char *in_hand = strchr(text, '\n'), *perks = NULL;
        if (in_hand != NULL) {
            *in_hand++ = '\0';
            perks = strchr(in_hand, '\n');
            if (perks != NULL)
                *perks++ = '\0';
        }
        static loadout_t loadout;
        loadout_parse(text, in_hand, perks, &loadout);
        char out[4096];
        if (loadout_format_guns(&loadout, out, sizeof(out)))
            must_end(out, sizeof(out));
        if (loadout_format_perks(&loadout, out, sizeof(out)))
            must_end(out, sizeof(out));
        break;
    }
    case 4: { // the rest of the loadout's texts
        static loadout_t loadout;
        loadout_clear(&loadout);
        loadout_parse_pap_levels(text, &loadout);
        loadout_parse_items(text, &loadout);
        loadout_parse_gun_ammo(text, &loadout);
        loadout_parse_hero_power(text, &loadout);
        loadout_parse_shield_health(text, &loadout);
        char out[4096];
        loadout_format_pap_levels(&loadout, out, sizeof(out));
        loadout_format_items(&loadout, out, sizeof(out));
        loadout_format_gun_ammo(&loadout, out, sizeof(out));
        loadout_format_hero_power(&loadout, out, sizeof(out));
        loadout_format_shield_health(&loadout, out, sizeof(out));
        loadout_valid_name(text);
        loadout_other_name(text, out, sizeof(out));
        break;
    }
    case 5: { // displays.ini, and the engine's description of the game's video settings
        static DisplayStore store;
        memset(&store, 0, sizeof(store));
        display_store_load(&store, path);
        GameValues game;
        display_policy_parse_game(text, &game);
        display_policy_persisted(text, which);
        break;
    }
    }
    free(text);
    return 0;
}

void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    static const char settings[] = "\x00; BO3MacFix\nJobThreads = 4\nOfflineMode = \"1\"\nBatteryFPS=#28\r\n";
    add(settings, sizeof(settings) - 1);
    static const char checkpoint[] = "\x01; BO3MacFix's zombies checkpoint\nRound = 12\nPoints = 4500\nMap = zm_theater\n"
                                     "Perks = juggernaut,quick_revive\nSaved = 1700000000\nApplied = 1\nPointsLater = 9000\n"
                                     "Guns = ar_standard+reflex:1:2,ray_gun\nGunInHand = ray_gun\nPerkList = specialty_armorvest\n";
    add(checkpoint, sizeof(checkpoint) - 1);
    static const char cfg[] = "\x02modvar tfoption_start_round 1\r\nmodvar tfoption_perkaholic \"1\"\r\n";
    add(cfg, sizeof(cfg) - 1);
    static const char guns[] = "\x03" "ar_standard+reflex+extbarrel:3:0,ray_gun_upgraded\nray_gun_upgraded\n"
                               "specialty_armorvest,specialty_quickrevive";
    add(guns, sizeof(guns) - 1);
    static const char rest[] = "\x04ar_standard:2,ray_gun:1";
    add(rest, sizeof(rest) - 1);
    static const char displays[] = "\x05[37D8832A-2D66-02CA-B9F7-8F30A301B230]\nname = Built-in Retina Display\nwidth = 3456\n"
                                   "height = 2234\nrefresh = 120\n[LastDisplay]\nkey = 37D8832A-2D66-02CA-B9F7-8F30A301B230\n"
                                   "game=2624x1696 refresh=120 maxfps=120";
    add(displays, sizeof(displays) - 1);
}
void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    add(" = ", 3);
    add("\r\n", 2);
    add("Round", 5);
    add("Points", 6);
    add("Guns", 4);
    add("modvar ", 7);
    add("game=", 5);
    add("refresh=", 8);
    add("[LastDisplay]", 13);
}
