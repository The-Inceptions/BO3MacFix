// Tests for checkpoint.c: AAE's custom mutations lines, and saving, applying and clearing a checkpoint, with made-up
// files in a temporary folder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/checkpoint.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char root[] = "/tmp/bo3mf_checkpoint_XXXXXX";
static char checkpoint_path[1024], players[1024], mutations[1024];
static checkpoint_paths_t paths;

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        printf("FAIL: couldn't write %s\n", path);
        exit(1);
    }
    fputs(text, file);
    fclose(file);
}

static const char *read_text(const char *path) {
    static char text[65536];
    text[0] = '\0';
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    size_t len = fread(text, 1, sizeof(text) - 1, file);
    text[len] = '\0';
    fclose(file);
    return text;
}

// the value text sets for dvar, "(none)" if nothing does
static const char *value_of(const char *text, const char *dvar) {
    static char out[256];
    return aae_cfg_value(text, dvar, out, sizeof(out)) ? out : "(none)";
}

static void test_cfg_value(void) {
    const char *text = "modvar tfoption_starting_round 7\r\n"
                       "modvar tfoption_starting_round_extra 9\n"
                       "  MODVAR\tTFOPTION_STARTING_POINTS   \"500\"  \n"
                       "modvar tfoption_starting_round 8";
    CHECK(strcmp(value_of(text, "tfoption_starting_round"), "8") == 0); // the last line wins
    CHECK(strcmp(value_of(text, "tfoption_starting_points"), "500") == 0); // any case, spaces and quotes
    CHECK(strcmp(value_of(text, "tfoption_starting_round_extra"), "9") == 0);
    CHECK(strcmp(value_of(text, "tfoption_starting"), "(none)") == 0); // a name that only starts the same
    CHECK(strcmp(value_of("modvar tfoption_x \"\"\n", "tfoption_x"), "") == 0);
    CHECK(strcmp(value_of("", "tfoption_x"), "(none)") == 0);
    char small[2];
    CHECK(!aae_cfg_value("modvar tfoption_x 123\n", "tfoption_x", small, sizeof(small)));
}

static void test_cfg_set(void) {
    char out[1024];
    // the first line that sets it gets the value, later ones go, every other line stays exactly as it was
    const char *text = "modvar tfoption_max_ammo 1\n"
                       "modvar tfoption_starting_round 7\n"
                       "// comment\n"
                       "modvar tfoption_starting_round 9\n"
                       "modvar tfoption_perkaholic 1\n";
    CHECK(aae_cfg_set(text, "tfoption_starting_round", "15", out, sizeof(out)));
    CHECK(strcmp(out, "modvar tfoption_max_ammo 1\n"
                      "modvar tfoption_starting_round 15\n"
                      "// comment\n"
                      "modvar tfoption_perkaholic 1\n") == 0);
    // added at the end when nothing sets it, with a line ending first if the file has none
    CHECK(aae_cfg_set("modvar tfoption_max_ammo 1", "tfoption_starting_points", "2500", out, sizeof(out)));
    CHECK(strcmp(out, "modvar tfoption_max_ammo 1\nmodvar tfoption_starting_points 2500\n") == 0);
    CHECK(aae_cfg_set("", "tfoption_starting_points", "2500", out, sizeof(out)));
    CHECK(strcmp(out, "modvar tfoption_starting_points 2500\n") == 0);
    // \r\n kept, for the line it replaces and one it adds
    CHECK(aae_cfg_set("modvar a 1\r\nmodvar tfoption_starting_round 7\r\n", "tfoption_starting_round", "3", out,
                      sizeof(out)));
    CHECK(strcmp(out, "modvar a 1\r\nmodvar tfoption_starting_round 3\r\n") == 0);
    CHECK(aae_cfg_set("modvar a 1\r\n", "tfoption_starting_round", "3", out, sizeof(out)));
    CHECK(strcmp(out, "modvar a 1\r\nmodvar tfoption_starting_round 3\r\n") == 0);
    // a last line without a line ending keeps having none
    CHECK(aae_cfg_set("modvar a 1\nmodvar tfoption_starting_round 7", "tfoption_starting_round", "3", out,
                      sizeof(out)));
    CHECK(strcmp(out, "modvar a 1\nmodvar tfoption_starting_round 3") == 0);
    // NULL takes it out
    CHECK(aae_cfg_set(text, "tfoption_starting_round", NULL, out, sizeof(out)));
    CHECK(strcmp(out, "modvar tfoption_max_ammo 1\n// comment\nmodvar tfoption_perkaholic 1\n") == 0);
    // a name that only starts the same isn't touched
    CHECK(aae_cfg_set("modvar tfoption_starting_round_x 1\n", "tfoption_starting_round", NULL, out, sizeof(out)));
    CHECK(strcmp(out, "modvar tfoption_starting_round_x 1\n") == 0);
    char small[16];
    CHECK(!aae_cfg_set(text, "tfoption_starting_round", "15", small, sizeof(small)));
}

static void test_save(void) {
    unlink(checkpoint_path);
    // Round and Points are needed, as whole numbers
    CHECK(!checkpoint_save(&paths, "Map=zm_theater\nPoints=100\n", 1));
    CHECK(!checkpoint_save(&paths, "Round=abc\nPoints=100\n", 1));
    CHECK(!checkpoint_save(&paths, "Round=0\nPoints=100\n", 1));
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=-1\n", 1));
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=100\njunk\n", 1)); // a line that isn't Key=value
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=100\nWeapon=a\tb\n", 1)); // a control character
    CHECK(read_text(checkpoint_path) == NULL);
    CHECK(checkpoint_save(&paths, "Map=zm_theater\nRound=23\nPoints=12340\nPerks=juggernaut,quick_revive\n"
                                  "Weapon=Ray Gun Mark II (Pack-a-Punched)\nStats=Kills 812, Downs 1\nApplied=1\n",
                          1760000000));
    char text[4096];
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Map=zm_theater\n") != NULL);
    CHECK(strstr(text, "Round=23\n") != NULL);
    CHECK(strstr(text, "Weapon=Ray Gun Mark II (Pack-a-Punched)\n") != NULL);
    CHECK(strstr(text, "Saved=1760000000\n") != NULL);
    CHECK(strstr(text, "Applied") == NULL); // only the checkpoint's own keys come from the page
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_No_Resume);
    // a new save replaces the fields, leaving out the ones it doesn't have
    CHECK(checkpoint_save(&paths, "Round=24\nPoints=100\n", 1760000100));
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Round=24\n") != NULL && strstr(text, "Map=") == NULL);
}

static const char *user_mutations = "modvar tfoption_max_ammo 1\n"
                                    "modvar tfoption_starting_round 7\n"
                                    "modvar tfoption_perkaholic 1\n"
                                    "modvar tfoption_starting_points 500\n"
                                    "modvar tfoption_loadoutsave 1\n";

static void test_apply_and_clear(void) {
    char commands[512];
    write_text(mutations, user_mutations);
    unlink(checkpoint_path);
    CHECK(!checkpoint_apply(&paths, "_Default", "7", "500", commands, sizeof(commands))); // no checkpoint yet
    CHECK(!checkpoint_mark_in_use(&paths));
    CHECK(!checkpoint_clear(&paths, "7", "500", commands, sizeof(commands))); // nothing to clear
    CHECK(checkpoint_save(&paths, "Map=zm_theater\nRound=23\nPoints=12340\n", 1));

    // malformed arguments change nothing
    CHECK(!checkpoint_apply(&paths, "_Default", "7; quit", "500", commands, sizeof(commands)));
    CHECK(!checkpoint_apply(&paths, "/../x", "7", "500", commands, sizeof(commands)));
    CHECK(!checkpoint_apply(&paths, "", "7", "500", commands, sizeof(commands)));
    CHECK(strcmp(read_text(mutations), user_mutations) == 0);
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_No_Resume);

    CHECK(checkpoint_apply(&paths, "_Default", "7", "500", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 23\nmodvar tfoption_starting_points 12340\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_max_ammo 1\n"
                                       "modvar tfoption_starting_round 23\n"
                                       "modvar tfoption_perkaholic 1\n"
                                       "modvar tfoption_starting_points 12340\n"
                                       "modvar tfoption_loadoutsave 1\n") == 0);
    CHECK(checkpoint_resume_state(&paths) != Checkpoint_No_Resume);
    // again (the game's options are the checkpoint's now): the first previous values stay
    CHECK(checkpoint_apply(&paths, "_Default", "23", "12340", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 23\nmodvar tfoption_starting_points 12340\n") == 0);
    // a game starts with it; saving then keeps it set up
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_Set_Up);
    CHECK(checkpoint_mark_in_use(&paths));
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_In_Use);
    CHECK(!checkpoint_mark_in_use(&paths));
    CHECK(checkpoint_save(&paths, "Map=zm_theater\nRound=25\nPoints=20000\n", 2));
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_In_Use);

    CHECK(checkpoint_clear(&paths, "23", "12340", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 7\nmodvar tfoption_starting_points 500\n") == 0);
    CHECK(strcmp(read_text(mutations), user_mutations) == 0);
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_No_Resume);
    CHECK(!checkpoint_clear(&paths, "23", "12340", commands, sizeof(commands)));
    char text[4096];
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Round=25\n") != NULL); // the checkpoint stays for next time

    // unset before: the lines go again, and the commands unset them; a value changed since is the player's
    write_text(mutations, "modvar tfoption_max_ammo 1\n");
    CHECK(checkpoint_apply(&paths, "_Default", "", "", commands, sizeof(commands)));
    CHECK(strcmp(read_text(mutations), "modvar tfoption_max_ammo 1\nmodvar tfoption_starting_round 25\n"
                                       "modvar tfoption_starting_points 20000\n") == 0);
    write_text(mutations, "modvar tfoption_max_ammo 1\nmodvar tfoption_starting_round 30\n"
                          "modvar tfoption_starting_points 20000\n");
    CHECK(checkpoint_clear(&paths, "30", "20000", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_points \"\"\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_max_ammo 1\nmodvar tfoption_starting_round 30\n") == 0);

    // rounds past AAE's slider are its last, and no mutations file yet gets one
    unlink(mutations);
    CHECK(checkpoint_save(&paths, "Round=300\nPoints=99999999\n", 3));
    CHECK(checkpoint_apply(&paths, "_Default", "", "", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 255\nmodvar tfoption_starting_points 9999990\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_starting_round 255\n"
                                       "modvar tfoption_starting_points 9999990\n") == 0);
    // at start-up there's no game to give commands to
    CHECK(checkpoint_clear(&paths, NULL, NULL, commands, sizeof(commands)));
    CHECK(strcmp(commands, "") == 0);
    CHECK(strcmp(read_text(mutations), "") == 0);
}

int main(void) {
    if (mkdtemp(root) == NULL) {
        printf("FAIL: couldn't make a temporary folder\n");
        return 1;
    }
    snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/checkpoint.ini", root);
    snprintf(players, sizeof(players), "%s/", root);
    snprintf(mutations, sizeof(mutations), "%sAAECustomMutations_Default.cfg", players);
    paths.checkpoint = checkpoint_path;
    paths.players = players;

    test_cfg_value();
    test_cfg_set();
    test_save();
    test_apply_and_clear();

    unlink(checkpoint_path);
    unlink(mutations);
    rmdir(root);
    if (failures == 0)
        printf("test_checkpoint: all passed\n");
    return failures == 0 ? 0 : 1;
}
