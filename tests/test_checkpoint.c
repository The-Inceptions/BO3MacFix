// Tests for checkpoint.c: AAE's custom mutations lines, saving a game, setting up its resume (round, points, perks)
// and undoing it, and the commands that give its weapons back, with made-up files in a temporary folder.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=100\nHeld=a\tb\n", 1)); // a control character
    // weapon and perk lists are names only, since weapons go into console commands
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=100\nWeapons=ray_gun;quit\n", 1));
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=100\nWeapons=ray gun\n", 1));
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=100\nWeapons=ray_gun,,thundergun\n", 1));
    CHECK(!checkpoint_save(&paths, "Round=5\nPoints=100\nPerks=juggernaut,\n", 1));
    CHECK(read_text(checkpoint_path) == NULL);
    CHECK(checkpoint_save(&paths, "Map=zm_theater\nRound=23\nPoints=12340\nPerks=juggernaut,quick_revive\n"
                                  "Weapons=ray_gun_upgraded,ar_standard+reflex\nEquipment=frag_grenade,knife\n"
                                  "Held=Ray Gun (Pack-a-Punched)\nCharacter=Richtofen\nStats=Kills 812, Downs 1\n"
                                  "Applied=1\n",
                          1760000000));
    char text[4096];
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Map=zm_theater\n") != NULL);
    CHECK(strstr(text, "Round=23\n") != NULL);
    CHECK(strstr(text, "Weapons=ray_gun_upgraded,ar_standard+reflex\n") != NULL);
    CHECK(strstr(text, "Held=Ray Gun (Pack-a-Punched)\n") != NULL);
    CHECK(strstr(text, "Character=Richtofen\n") != NULL);
    CHECK(strstr(text, "Saved=1760000000\n") != NULL);
    CHECK(strstr(text, "Applied") == NULL); // only the saved game's own keys come from the page
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_No_Resume);
    // a new save replaces the fields, leaving out the ones it doesn't have
    CHECK(checkpoint_save(&paths, "Round=24\nPoints=100\n", 1760000100));
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Round=24\n") != NULL && strstr(text, "Map=") == NULL && strstr(text, "Weapons=") == NULL);
}

// what's printed to stdout between capture_start and capture_end
static char capture_path[1100];
static int stdout_copy = -1;
static void capture_start(void) {
    fflush(stdout);
    stdout_copy = dup(1);
    int file = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(file, 1);
    close(file);
}
static char *capture_end(void) {
    fflush(stdout);
    dup2(stdout_copy, 1);
    close(stdout_copy);
    const char *text = read_text(capture_path);
    unlink(capture_path);
    return strdup(text != NULL ? text : "");
}

// a checkpoint.ini with count entries: a set-up resume's (InUse, Applied, AppliedPreset, the options' Applied and
// Previous values), the saved game's, then made-up ones (a newer BO3MacFix's keys)
static void write_store(int count) {
    static const char *const resume[] = { "InUse = 1",       "Applied = 1",       "AppliedPreset = _Default",
                                          "AppliedRound = 38", "PreviousRound = 1", "AppliedPoints = 500",
                                          "PreviousPoints = ", "AppliedPerkaholic = 1", "PreviousPerkaholic = 0",
                                          "AppliedQuickRevive = 1", "PreviousQuickRevive = 0", "Saved = 1",
                                          "Round = 38",       "Points = 500",      "Held = Ray Gun" };
    static char text[65536];
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        if (i < (int)(sizeof(resume) / sizeof(resume[0])))
            used += (size_t)snprintf(text + used, sizeof(text) - used, "%s\n", resume[i]);
        else
            used += (size_t)snprintf(text + used, sizeof(text) - used, "Newer%c%c = %d\n", 'a' + i / 26, 'a' + i % 26, i);
    }
    write_text(checkpoint_path, text);
}

// checkpoint.ini's room: 28 keys in use (a save's 16 and Saved, a resume's 11), more for later; a save that needs one
// more than it holds is refused and logged, the file as it was
static void test_store_room(void) {
    snprintf(capture_path, sizeof(capture_path), "%s/stdout.txt", root);
    const char *full_save = "Map=zm_theater\nRound=39\nPoints=600\nPerks=juggernaut\nWeapons=ray_gun_upgraded\n"
                            "Equipment=frag_grenade\nHeld=Ray Gun\nCharacter=Dempsey\nStats=Kills 9\n"
                            "Guns=ray_gun_upgraded:0:0:\nGunInHand=ray_gun_upgraded\nPerkList=specialty_armorvest\n"
                            "PapLevels=ray_gun_upgraded:3\nItems=frag_grenade:2:0\nGunAmmo=ray_gun_upgraded:7:44\n"
                            "HeroPower=hero_annihilator:40\n";
    // a resumed game's save with every key, and 8 entries from a newer version: all of them stay
    write_store(15 + 8);
    CHECK(checkpoint_save(&paths, full_save, 2));
    char text[Checkpoint_Read_Size];
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Applied=1\n") != NULL && strstr(text, "PreviousRound=1\n") != NULL &&
          strstr(text, "AppliedPreset=_Default\n") != NULL && strstr(text, "HeroPower=hero_annihilator:40\n") != NULL &&
          strstr(text, "Round=39\n") != NULL && strstr(text, "Saved=2\n") != NULL);
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_In_Use);
    int entries = 0;
    for (const char *line = text; (line = strchr(line, '\n')) != NULL; line++)
        entries++;
    CHECK(entries == 15 + 8 + 13); // Held was there already; the save's other 13 are new
    // full: a save that needs one more entry isn't saved, and says why
    write_store(Checkpoint_Max_Entries);
    char *before = strdup(read_text(checkpoint_path));
    capture_start();
    bool saved = checkpoint_save(&paths, "Map=zm_theater\nRound=39\nPoints=600\nHeld=Ray Gun\n", 3);
    char *log = capture_end();
    CHECK(!saved);
    CHECK(strcmp(read_text(checkpoint_path), before) == 0);
    CHECK(strstr(log, "checkpoint.ini has no room for Map (it holds 64 entries), so it isn't changed") != NULL);
    free(log);
    // the keys a save doesn't have go first, so one that drops Held has room for Map
    CHECK(checkpoint_save(&paths, "Map=zm_theater\nRound=39\nPoints=600\n", 4));
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Map=zm_theater\n") != NULL && strstr(text, "Held=") == NULL &&
          strstr(text, "Applied=1\n") != NULL && strstr(text, "Saved=4\n") != NULL);
    // a file with more entries than that (a newer version's) isn't replaced either: it may have a resume set up
    write_store(Checkpoint_Max_Entries + 1);
    free(before);
    before = strdup(read_text(checkpoint_path));
    capture_start();
    saved = checkpoint_save(&paths, full_save, 5);
    log = capture_end();
    CHECK(!saved && strcmp(read_text(checkpoint_path), before) == 0 && strstr(log, "checkpoint.ini has more entries than the 64 it holds, so it isn't changed") != NULL);
    // and its readers just don't read it, without a word
    char state_text[64];
    capture_start();
    checkpoint_resume_t state = checkpoint_resume_state(&paths);
    checkpoint_read(&paths, state_text, sizeof(state_text));
    free(log);
    log = capture_end();
    CHECK(state == Checkpoint_No_Resume && state_text[0] == '\0' && log[0] == '\0');
    free(log);
    // nor does the resume's InUse go in when there's no room for it
    write_store(Checkpoint_Max_Entries);
    char *full = calloc(1, 65536);
    strcpy(full, read_text(checkpoint_path));
    char *in_use = strstr(full, "InUse = 1\n");
    CHECK(in_use != NULL);
    memmove(in_use, in_use + strlen("InUse = 1\n"), strlen(in_use + strlen("InUse = 1\n")) + 1);
    strcat(full, "Newerzz = 1\n"); // full again, without InUse
    write_text(checkpoint_path, full);
    free(before);
    before = strdup(read_text(checkpoint_path));
    capture_start();
    saved = checkpoint_mark_in_use(&paths);
    log = capture_end();
    CHECK(!saved && strcmp(read_text(checkpoint_path), before) == 0 && strstr(log, "no room for InUse") != NULL);
    free(log);
    free(before);
    free(full);
    unlink(checkpoint_path);
}

static const char *user_mutations = "modvar tfoption_max_ammo 1\n"
                                    "modvar tfoption_starting_round 7\n"
                                    "modvar tfoption_perkaholic 1\n"
                                    "modvar tfoption_starting_points 500\n"
                                    "modvar tfoption_loadoutsave 1\n";

static const char *game_values = "tfoption_starting_round=23\ntfoption_starting_points=12340\n"
                                 "tfoption_perkaholic=1\ntfoption_spawn_with_quick_res=\n";

static void test_apply_and_clear(void) {
    char commands[512];
    write_text(mutations, user_mutations);
    unlink(checkpoint_path);
    CHECK(!checkpoint_apply(&paths, "_Default", commands, sizeof(commands))); // no checkpoint yet
    CHECK(!checkpoint_mark_in_use(&paths));
    CHECK(!checkpoint_clear(&paths, game_values, commands, sizeof(commands))); // nothing to clear
    CHECK(checkpoint_save(&paths, "Map=zm_theater\nRound=23\nPoints=12340\nPerks=juggernaut\n", 1));

    // malformed presets change nothing
    CHECK(!checkpoint_apply(&paths, "/../x", commands, sizeof(commands)));
    CHECK(!checkpoint_apply(&paths, "", commands, sizeof(commands)));
    CHECK(!checkpoint_apply(&paths, "_Default;quit", commands, sizeof(commands)));
    CHECK(strcmp(read_text(mutations), user_mutations) == 0);
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_No_Resume);

    // the round and points; Perkaholic is on already, so it's left alone
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 23\nmodvar tfoption_starting_points 12340\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_max_ammo 1\n"
                                       "modvar tfoption_starting_round 23\n"
                                       "modvar tfoption_perkaholic 1\n"
                                       "modvar tfoption_starting_points 12340\n"
                                       "modvar tfoption_loadoutsave 1\n") == 0);
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_Set_Up);
    // again (the file has the checkpoint's values now): the values from before the first time stay
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 23\nmodvar tfoption_starting_points 12340\n") == 0);
    // a game starts with it and gets its weapons; saving then keeps it set up
    CHECK(!checkpoint_mark_weapons_given(&paths)); // not before a game has started with it
    CHECK(checkpoint_mark_in_use(&paths));
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_In_Use);
    CHECK(!checkpoint_mark_in_use(&paths));
    CHECK(checkpoint_mark_weapons_given(&paths));
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_Weapons_Given);
    CHECK(!checkpoint_mark_weapons_given(&paths));
    CHECK(checkpoint_save(&paths, "Map=zm_theater\nRound=25\nPoints=20000\n", 2));
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_Resume_Weapons_Given);

    CHECK(checkpoint_clear(&paths, game_values, commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 7\nmodvar tfoption_starting_points 500\n") == 0);
    CHECK(strcmp(read_text(mutations), user_mutations) == 0);
    CHECK(checkpoint_resume_state(&paths) == Checkpoint_No_Resume);
    CHECK(!checkpoint_clear(&paths, game_values, commands, sizeof(commands)));
    char text[4096];
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Round=25\n") != NULL); // the saved game stays for next time
    CHECK(strstr(text, "Applied") == NULL && strstr(text, "Previous") == NULL && strstr(text, "InUse") == NULL);

    // unset before: the lines go again, and the commands unset them; a value changed since is the player's
    write_text(mutations, "modvar tfoption_max_ammo 1\n");
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strcmp(read_text(mutations), "modvar tfoption_max_ammo 1\nmodvar tfoption_starting_round 25\n"
                                       "modvar tfoption_starting_points 20000\n") == 0);
    write_text(mutations, "modvar tfoption_max_ammo 1\nmodvar tfoption_starting_round 30\n"
                          "modvar tfoption_starting_points 20000\n");
    CHECK(checkpoint_clear(&paths, "tfoption_starting_round=30\r\ntfoption_starting_points=20000", commands,
                           sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_points \"\"\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_max_ammo 1\nmodvar tfoption_starting_round 30\n") == 0);

    // rounds past AAE's slider are its last, and no mutations file yet gets one
    unlink(mutations);
    CHECK(checkpoint_save(&paths, "Round=300\nPoints=99999999\n", 3));
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 255\nmodvar tfoption_starting_points 9999990\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_starting_round 255\n"
                                       "modvar tfoption_starting_points 9999990\n") == 0);
    // at start-up there's no game to give commands to
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));
    CHECK(strcmp(commands, "") == 0);
    CHECK(strcmp(read_text(mutations), "") == 0);
}

static void test_perks(void) {
    char commands[512];
    // perks but Perkaholic off: it goes on, with Quick Revive for that one, and both go back after
    write_text(mutations, "modvar tfoption_perkaholic 0\nmodvar tfoption_starting_round 7\n");
    unlink(checkpoint_path);
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nPerks=quick_revive,doubletap2\n", 1));
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 12\nmodvar tfoption_starting_points 4000\n"
                           "modvar tfoption_perkaholic 1\nmodvar tfoption_spawn_with_quick_res 1\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_perkaholic 1\nmodvar tfoption_starting_round 12\n"
                                       "modvar tfoption_starting_points 4000\n"
                                       "modvar tfoption_spawn_with_quick_res 1\n") == 0);
    CHECK(checkpoint_clear(&paths, "tfoption_starting_round=12\ntfoption_starting_points=4000\n"
                                   "tfoption_perkaholic=1\ntfoption_spawn_with_quick_res=1\n",
                           commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 7\nmodvar tfoption_starting_points \"\"\n"
                           "modvar tfoption_perkaholic 0\nmodvar tfoption_spawn_with_quick_res \"\"\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_perkaholic 0\nmodvar tfoption_starting_round 7\n") == 0);

    // Quick Revive alone: only that, never Perkaholic
    CHECK(checkpoint_save(&paths, "Round=3\nPoints=900\nPerks=quick_revive\n", 2));
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strstr(commands, "tfoption_perkaholic") == NULL);
    CHECK(strstr(commands, "modvar tfoption_spawn_with_quick_res 1\n") != NULL);
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));

    // no perks: the player's own perk options stay as they are
    CHECK(checkpoint_save(&paths, "Round=3\nPoints=900\nPerks=\n", 3));
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 3\nmodvar tfoption_starting_points 900\n") == 0);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_perkaholic 0\nmodvar tfoption_starting_round 3\n"
                                       "modvar tfoption_starting_points 900\n") == 0);

    // set up again after a save without perks: Perkaholic, turned on the first time, goes back now
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));
    CHECK(checkpoint_save(&paths, "Round=3\nPoints=900\nPerks=juggernaut\n", 4));
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strstr(read_text(mutations), "modvar tfoption_perkaholic 1\n") != NULL);
    CHECK(checkpoint_save(&paths, "Round=4\nPoints=900\n", 5));
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strstr(commands, "modvar tfoption_perkaholic 0\n") != NULL);
    CHECK(strcmp(read_text(mutations), "modvar tfoption_perkaholic 0\nmodvar tfoption_starting_round 4\n"
                                       "modvar tfoption_starting_points 900\n") == 0);
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));
    CHECK(strcmp(read_text(mutations), "modvar tfoption_perkaholic 0\nmodvar tfoption_starting_round 7\n") == 0);
}

// the full loadout: saved when it reads back, given to zm_inventory.c from the file, and AAE's perk options left alone
static void test_loadout(void) {
    char commands[512], text[4096];
    loadout_t loadout;
    write_text(mutations, "modvar tfoption_perkaholic 0\nmodvar tfoption_starting_round 7\n");
    unlink(checkpoint_path);
    CHECK(!checkpoint_loadout(&paths, &loadout)); // no checkpoint
    const char *guns = "ar_standard_upgraded_zm+reflex+extclip:822a:17:zm_aat_dead_wire,ray_gun_upgraded_zm:2a:0:";
    char lines[1024];
    snprintf(lines, sizeof(lines), "Round=12\nPoints=4000\nPerks=juggernaut,doubletap2\nGuns=%s\n"
             "GunInHand=ray_gun_upgraded_zm\nPerkList=specialty_armorvest,specialty_doubletap2\n", guns);
    CHECK(checkpoint_save(&paths, lines, 1));
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "GunInHand=ray_gun_upgraded_zm\n") != NULL);
    CHECK(strstr(text, "PerkList=specialty_armorvest,specialty_doubletap2\n") != NULL);
    CHECK(checkpoint_loadout(&paths, &loadout));
    CHECK(loadout.gun_count == 2 && loadout.held == 1 && loadout.perk_count == 2);
    CHECK(strcmp(loadout.guns[0].name, "ar_standard_upgraded_zm+reflex+extclip") == 0 &&
          loadout.guns[0].options == 0x822a && loadout.guns[0].acvi == 17 &&
          strcmp(loadout.guns[0].aat, "zm_aat_dead_wire") == 0);

    // the exact perks come back from give_perk, so Perkaholic and Quick Revive stay the player's
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strcmp(commands, "modvar tfoption_starting_round 12\nmodvar tfoption_starting_points 4000\n") == 0);
    CHECK(strstr(read_text(mutations), "tfoption_perkaholic 0") != NULL);
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));
    // perks only (no guns read): still a loadout, and still no Perkaholic
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nPerks=juggernaut\nPerkList=specialty_armorvest\n", 2));
    CHECK(checkpoint_loadout(&paths, &loadout) && loadout.gun_count == 0 && loadout.perk_count == 1);
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strstr(commands, "perkaholic") == NULL);
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));
    // an older save without them: no loadout, and Perkaholic for its perks as before
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nPerks=juggernaut\n", 3));
    CHECK(!checkpoint_loadout(&paths, &loadout));
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(strstr(commands, "modvar tfoption_perkaholic 1\n") != NULL);
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));

    // a loadout that doesn't read back isn't saved
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_zm:zz:0:\n", 4));
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_zm;quit:0:0:\n", 4));
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nPerkList=specialty armorvest\n", 4));
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGunInHand=ray gun\n", 4));
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_zm:0:0:\nPapLevels=ray_gun_zm:9\n", 4));
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Saved=3\n") != NULL); // the last good save stays
    CHECK(strstr(text, "PapLevels") == NULL);
    // AAE's repack levels go with their guns, and a save without them drops the last one's
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:,pistol_m1911:0:0:\n"
                                  "PapLevels=ray_gun_upgraded:4\n", 5));
    CHECK(checkpoint_loadout(&paths, &loadout) && loadout.gun_count == 2);
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "PapLevels=ray_gun_upgraded:4\n") != NULL);
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\n", 6));
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "PapLevels") == NULL);
    // the items and the guns' ammo too, read back for the restore
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nPapLevels=ray_gun_upgraded:2\n"
                                  "Items=frag_grenade:3:0,zod_riotshield::\nGunAmmo=ray_gun_upgraded:12:160\n", 7));
    CHECK(checkpoint_loadout(&paths, &loadout) && loadout.gun_count == 1 && loadout.guns[0].pap_level == 2 &&
          loadout.item_count == 2 && loadout.items[0].clip == 3 && !loadout.items[1].have_ammo &&
          loadout.guns[0].have_ammo && loadout.guns[0].stock == 160);
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nItems=frag grenade:3:0\n", 8));
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nGunAmmo=ray_gun_upgraded::\n",
                           8));
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "Saved=7\n") != NULL && strstr(text, "Items=frag_grenade:3:0,zod_riotshield::\n") != NULL);
    // the hero weapon's charge: kept, read back for the restore; refused when malformed; gone with a save without it
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nItems=hero_annihilator::\n"
                                  "HeroPower=hero_annihilator:40\n", 9));
    CHECK(checkpoint_loadout(&paths, &loadout) && loadout.have_hero_power && loadout.hero_power == 40);
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nHeroPower=hero_annihilator:400\n",
                           10));
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\n", 11));
    CHECK(checkpoint_loadout(&paths, &loadout) && !loadout.have_hero_power);
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "HeroPower") == NULL);
    // the shield's health, the same way
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nItems=zod_riotshield::\n"
                                  "ShieldHealth=zod_riotshield:740\n", 12));
    CHECK(checkpoint_loadout(&paths, &loadout) && loadout.have_shield_health && loadout.shield_health == 740 &&
          strcmp(loadout.shield_name, "zod_riotshield") == 0);
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "ShieldHealth=zod_riotshield:740\n") != NULL);
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nShieldHealth=ray_gun:40\n", 13));
    CHECK(!checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\nShieldHealth=zod_riotshield:x\n",
                           13));
    CHECK(checkpoint_loadout(&paths, &loadout) && loadout.shield_health == 740); // the refused saves changed nothing
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nGuns=ray_gun_upgraded:0:0:\n", 14));
    CHECK(checkpoint_loadout(&paths, &loadout) && !loadout.have_shield_health);
    checkpoint_read(&paths, text, sizeof(text));
    CHECK(strstr(text, "ShieldHealth") == NULL);
    CHECK(checkpoint_save(&paths, "Round=12\nPoints=4000\nPerks=juggernaut\n", 3)); // as the test below expects
    // the longest a Guns line gets: four guns with eight attachments each fit in a value
    char long_guns[1024] = "";
    for (int i = 0; i < 4; i++)
        snprintf(long_guns + strlen(long_guns), sizeof(long_guns) - strlen(long_guns),
                 "%sar_standard_upgraded_zm+acog+damage+dualclip+extbarrel+extclip+fastreload+fmj+gmod0:"
                 "ffffffffffffffff:4294967295:zm_aat_thunder_wall", i > 0 ? "," : "");
    snprintf(lines, sizeof(lines), "Round=12\nPoints=4000\nGuns=%s\n", long_guns);
    CHECK(strlen(long_guns) < 1000);
    CHECK(checkpoint_save(&paths, lines, 5));
    CHECK(checkpoint_loadout(&paths, &loadout) && loadout.gun_count == 4);
    unlink(checkpoint_path);
}

// the commands for saved and held, "(error)" if malformed
static const char *weapon_commands(const char *saved, const char *held, int *count) {
    static char out[1024];
    *count = checkpoint_weapon_commands(saved, held, out, sizeof(out));
    return *count >= 0 ? out : "(error)";
}

static void test_weapon_commands(void) {
    int count;
    // take what isn't saved first, then give what isn't held
    CHECK(strcmp(weapon_commands("ray_gun_upgraded,thundergun_upgraded", "pistol_m1911,pistol_burst", &count),
                 "take pistol_m1911\ntake pistol_burst\ngive ray_gun_upgraded\ngive thundergun_upgraded\n") == 0);
    CHECK(count == 4);
    // what's held already stays
    CHECK(strcmp(weapon_commands("pistol_burst,ray_gun", "pistol_m1911,pistol_burst", &count),
                 "take pistol_m1911\ngive ray_gun\n") == 0);
    CHECK(count == 2);
    CHECK(strcmp(weapon_commands("pistol_burst", "pistol_burst", &count), "") == 0 && count == 0);
    // nothing saved (an old save, or the weapons couldn't be read): the game's own weapons stay
    CHECK(strcmp(weapon_commands("", "pistol_m1911", &count), "") == 0 && count == 0);
    CHECK(strcmp(weapon_commands(NULL, "pistol_m1911", &count), "") == 0 && count == 0);
    // nothing held yet: all given; a name twice once; attachments kept
    CHECK(strcmp(weapon_commands("ar_standard+reflex,ray_gun,ray_gun", "", &count),
                 "give ar_standard+reflex\ngive ray_gun\n") == 0);
    CHECK(count == 2);
    // a name that only starts the same is another weapon
    CHECK(strcmp(weapon_commands("ray_gun_upgraded", "ray_gun", &count), "take ray_gun\ngive ray_gun_upgraded\n") ==
          0);
    // anything but names is refused, so nothing else can get into a command
    CHECK(strcmp(weapon_commands("ray_gun;quit", "", &count), "(error)") == 0);
    CHECK(strcmp(weapon_commands("ray_gun", "a b", &count), "(error)") == 0);
    char small[8];
    CHECK(checkpoint_weapon_commands("ray_gun_upgraded", "", small, sizeof(small)) == -1);
}

// the settings page's "3 h ago" for the checkpoint's Saved
static void test_saved_when(void) {
    char out[32];
    // a fixed time zone, so the dates below don't depend on the machine's
    setenv("TZ", "America/New_York", 1);
    tzset();
    const long now = 1792423531; // Mon Oct 19 2026, 11:25:31 EDT
    checkpoint_saved_when("1792423531", now, out, sizeof(out));
    CHECK(strcmp(out, "just now") == 0);
    checkpoint_saved_when("1792423472", now, out, sizeof(out)); // 59 s
    CHECK(strcmp(out, "just now") == 0);
    checkpoint_saved_when("1792423471", now, out, sizeof(out)); // 60 s
    CHECK(strcmp(out, "1 min ago") == 0);
    checkpoint_saved_when("1792420000", now, out, sizeof(out)); // 58 min 51 s
    CHECK(strcmp(out, "58 min ago") == 0);
    checkpoint_saved_when("1792419931", now, out, sizeof(out)); // 1 h
    CHECK(strcmp(out, "1 h ago") == 0);
    checkpoint_saved_when("1792337132", now, out, sizeof(out)); // 23 h 59 min 59 s
    CHECK(strcmp(out, "23 h ago") == 0);
    checkpoint_saved_when("1792337131", now, out, sizeof(out)); // a day: Oct 18
    CHECK(strcmp(out, "Oct 18") == 0);
    checkpoint_saved_when("1767225600", now, out, sizeof(out)); // Jan 1 2026, 00:00 UTC: Dec 31 2025 in New York
    CHECK(strcmp(out, "Dec 31 2025") == 0);
    checkpoint_saved_when("1767243600", now, out, sizeof(out)); // Jan 1 2026, 00:00 EST
    CHECK(strcmp(out, "Jan 1") == 0);
    // a few seconds ahead (clocks) is now; further ahead, or not a number, is nothing
    checkpoint_saved_when("1792423560", now, out, sizeof(out));
    CHECK(strcmp(out, "just now") == 0);
    checkpoint_saved_when("1792423600", now, out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    checkpoint_saved_when("12ab", now, out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    checkpoint_saved_when("", now, out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    checkpoint_saved_when(NULL, now, out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    checkpoint_saved_when("-5", now, out, sizeof(out));
    CHECK(strcmp(out, "") == 0);
    // a small buffer is cut, not overrun
    char tiny[4];
    checkpoint_saved_when("1792420000", now, tiny, sizeof(tiny));
    CHECK(strcmp(tiny, "58 ") == 0);
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
    test_store_room();
    test_apply_and_clear();
    test_perks();
    test_weapon_commands();
    test_loadout();
    test_saved_when();

    unlink(checkpoint_path);
    unlink(mutations);
    rmdir(root);
    if (failures == 0)
        printf("test_checkpoint: all passed\n");
    return failures == 0 ? 0 : 1;
}
