// Tests for checkpoint_autosave.c: the rule that keeps a deeper saved game, the background writer (the newest save it's
// given, at most once per interval, never over a resumed game's weapons), deleting the saved game, and the lock that
// keeps the writer and the UI's calls into checkpoint.c apart, with made-up files in a temporary folder.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../source/checkpoint_autosave.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char root[] = "/tmp/bo3mf_autosave_XXXXXX";
static char checkpoint_path[1024], players[1024], mutations[1024];
static checkpoint_paths_t paths;

// ---- what the writer reports

typedef struct {
    autosave_outcome_t outcome;
    long round, kept;
    char note[64];
    uint64_t at_ns;
} report_t;

static pthread_mutex_t reports_mutex = PTHREAD_MUTEX_INITIALIZER;
static report_t reports[64];
static int report_count;

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void report(autosave_outcome_t outcome, long round, long kept, const char *note) {
    pthread_mutex_lock(&reports_mutex);
    if (report_count < (int)(sizeof(reports) / sizeof(reports[0]))) {
        report_t *r = &reports[report_count++];
        r->outcome = outcome;
        r->round = round;
        r->kept = kept;
        snprintf(r->note, sizeof(r->note), "%s", note);
        r->at_ns = now_ns();
    }
    pthread_mutex_unlock(&reports_mutex);
}

static int reports_now(void) {
    pthread_mutex_lock(&reports_mutex);
    int count = report_count;
    pthread_mutex_unlock(&reports_mutex);
    return count;
}

static const report_t *last_report(void) {
    return report_count > 0 ? &reports[report_count - 1] : NULL;
}

// ---- files

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        printf("FAIL: couldn't write %s\n", path);
        exit(1);
    }
    fputs(text, file);
    fclose(file);
}

// a value of checkpoint.ini as checkpoint_read gives it, "(none)" if it isn't there
static const char *saved_value(const char *key) {
    static char lines[16384], value[256];
    checkpoint_lock();
    checkpoint_read(&paths, lines, sizeof(lines));
    checkpoint_unlock();
    size_t len = strlen(key);
    for (const char *line = lines; *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t line_len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (line_len > len && strncmp(line, key, len) == 0 && line[len] == '=') {
            snprintf(value, sizeof(value), "%.*s", (int)(line_len - len - 1), line + len + 1);
            return value;
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    return "(none)";
}

static long saved_round(void) {
    const char *round = saved_value("Round");
    return strcmp(round, "(none)") == 0 ? -1 : atol(round);
}

static checkpoint_resume_t resume_state(void) {
    checkpoint_lock();
    checkpoint_resume_t state = checkpoint_resume_state(&paths);
    checkpoint_unlock();
    return state;
}

// a game as checkpoint.lua gives it
static const char *game(long round, const char *weapons) {
    static char lines[512];
    snprintf(lines, sizeof(lines), "Map=zm_theater\nRound=%ld\nPoints=%ld\nPerks=juggernaut\nWeapons=%s\n"
             "Equipment=knife\nHeld=Ray Gun\n", round, round * 1000, weapons);
    return lines;
}

// posts a save and waits for the writer, true if it reported once more
static bool post_and_wait(const char *lines, const char *note) {
    int before = reports_now();
    bool posted = checkpoint_autosave_post(lines, note);
    checkpoint_autosave_flush();
    return posted && reports_now() == before + 1;
}

// ---- tests

static void test_may_replace(void) {
    const char *saved = "Map=zm_theater\nRound=12\nPoints=100\n";
    CHECK(checkpoint_may_replace("", 1, false));   // nothing saved
    CHECK(checkpoint_may_replace(NULL, 1, false));
    CHECK(checkpoint_may_replace(saved, 12, false)); // the same round, later
    CHECK(checkpoint_may_replace(saved, 13, false));
    CHECK(!checkpoint_may_replace(saved, 11, false)); // a deeper game this one didn't resume stays
    CHECK(!checkpoint_may_replace(saved, 1, false));
    CHECK(checkpoint_may_replace(saved, 11, true)); // unless it resumed it
    // only the Round line counts
    CHECK(checkpoint_may_replace("AppliedRound=40\nRounds=50\nRound=5\n", 6, false));
    CHECK(!checkpoint_may_replace("AppliedRound=4\nRound=50\n", 6, false));
    CHECK(checkpoint_may_replace("Round=abc\n", 1, false)); // an unreadable round is replaced, as checkpoint_save does
    CHECK(checkpoint_may_replace("Round=\n", 1, false));
    CHECK(checkpoint_may_replace("Round=-3\n", 1, false));
    CHECK(!checkpoint_may_replace("Round=30\r\n", 29, false)); // a line ending from Windows
}

static void test_writer(void) {
    checkpoint_autosave_init(&paths, 0, report);
    CHECK(checkpoint_autosave_enabled()); // on by default
    CHECK(post_and_wait(game(12, "ray_gun_upgraded"), "round 12 started"));
    const report_t *r = last_report();
    CHECK(r != NULL && r->outcome == Autosave_Saved && r->round == 12 && r->kept == 0);
    CHECK(r != NULL && strcmp(r->note, "round 12 started") == 0);
    CHECK(saved_round() == 12);
    CHECK(strcmp(saved_value("Weapons"), "ray_gun_upgraded") == 0);
    CHECK(strcmp(saved_value("Saved"), "(none)") != 0); // with the time it's saved at, like any save

    // a lower round doesn't replace it
    CHECK(post_and_wait(game(5, "pistol_m1911"), "round 5 started"));
    r = last_report();
    CHECK(r != NULL && r->outcome == Autosave_Kept_Deeper && r->round == 5 && r->kept == 12);
    CHECK(saved_round() == 12 && strcmp(saved_value("Weapons"), "ray_gun_upgraded") == 0);
    // the same round and higher ones do
    CHECK(post_and_wait(game(12, "ray_gun_upgraded,pistol_burst"), ""));
    CHECK(last_report()->outcome == Autosave_Saved);
    CHECK(strcmp(saved_value("Weapons"), "ray_gun_upgraded,pistol_burst") == 0);
    CHECK(post_and_wait(game(13, "ray_gun_upgraded,pistol_burst"), NULL));
    CHECK(last_report()->outcome == Autosave_Saved && saved_round() == 13);

    // malformed saves change nothing
    CHECK(post_and_wait("Map=zm_theater\nPoints=10\n", "no round"));
    CHECK(last_report()->outcome == Autosave_Failed);
    CHECK(post_and_wait("Round=x\nPoints=10\n", "a round that isn't a number"));
    CHECK(last_report()->outcome == Autosave_Failed);
    CHECK(post_and_wait("Round=20\nPoints=10\nWeapons=ray gun; quit\n", "a weapon that isn't a name"));
    CHECK(last_report()->outcome == Autosave_Failed);
    CHECK(saved_round() == 13);
    CHECK(!checkpoint_autosave_post(NULL, "nothing"));

    // AutoSave off takes nothing
    checkpoint_autosave_set_enabled(false);
    CHECK(!checkpoint_autosave_enabled());
    int before = reports_now();
    CHECK(!checkpoint_autosave_post(game(40, "ray_gun"), ""));
    checkpoint_autosave_flush();
    CHECK(reports_now() == before && saved_round() == 13);
    checkpoint_autosave_set_enabled(true);
}

static void test_resume(void) {
    write_text(mutations, "modvar tfoption_starting_round 7\nmodvar tfoption_starting_points 500\n");
    CHECK(post_and_wait(game(30, "ray_gun_upgraded,pistol_burst"), ""));
    CHECK(saved_round() == 30);
    char commands[512];
    checkpoint_lock();
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    CHECK(checkpoint_mark_in_use(&paths));
    checkpoint_unlock();
    CHECK(resume_state() == Checkpoint_Resume_In_Use);
    // the resumed game, before it has its weapons back: they're still to come from checkpoint.ini
    CHECK(post_and_wait(game(30, "pistol_m1911"), ""));
    CHECK(last_report()->outcome == Autosave_Resuming && last_report()->round == 30);
    CHECK(strcmp(saved_value("Weapons"), "ray_gun_upgraded,pistol_burst") == 0);
    checkpoint_lock();
    CHECK(checkpoint_mark_weapons_given(&paths));
    checkpoint_unlock();
    // after that it may save any round: it's the game this one resumed
    CHECK(post_and_wait(game(29, "ray_gun_upgraded"), ""));
    CHECK(last_report()->outcome == Autosave_Saved && saved_round() == 29);
    // and the resume stays set up, for the menus to undo
    CHECK(resume_state() == Checkpoint_Resume_Weapons_Given);
    CHECK(strcmp(saved_value("AppliedRound"), "30") == 0);
    checkpoint_lock();
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));
    checkpoint_unlock();
    // the next game didn't resume it
    CHECK(post_and_wait(game(28, "pistol_m1911"), ""));
    CHECK(last_report()->outcome == Autosave_Kept_Deeper && last_report()->kept == 29);
}

static void test_interval(void) {
    // the newest save, at most once per interval
    checkpoint_autosave_init(&paths, 300, report);
    CHECK(post_and_wait(game(40, "ray_gun"), "40"));
    int before = reports_now();
    uint64_t first = last_report()->at_ns;
    CHECK(checkpoint_autosave_post(game(41, "ray_gun"), "41"));
    CHECK(checkpoint_autosave_post(game(42, "ray_gun"), "42"));
    CHECK(checkpoint_autosave_post(game(43, "ray_gun"), "43"));
    checkpoint_autosave_flush();
    CHECK(reports_now() == before + 1);
    const report_t *r = last_report();
    CHECK(r->round == 43 && strcmp(r->note, "43") == 0 && saved_round() == 43);
    CHECK(r->at_ns - first >= 250 * 1000000ull); // the interval, give or take the timer's slack
    // waiting longer than the interval saves at once
    usleep(350 * 1000);
    uint64_t posted = now_ns();
    CHECK(post_and_wait(game(44, "ray_gun"), "44"));
    CHECK(last_report()->at_ns - posted < 200 * 1000000ull);

    // a manual save drops one the writer hasn't written yet
    before = reports_now();
    CHECK(checkpoint_autosave_post(game(45, "ray_gun"), "45"));
    checkpoint_lock();
    checkpoint_autosave_cancel();
    checkpoint_unlock();
    checkpoint_autosave_flush();
    CHECK(reports_now() == before && saved_round() == 44);
    checkpoint_autosave_init(&paths, 0, report);
}

static void test_delete(void) {
    char commands[512];
    CHECK(saved_round() > 0);
    // not while a resume is set up: checkpoint.ini has the player's own AAE options
    checkpoint_lock();
    CHECK(checkpoint_apply(&paths, "_Default", commands, sizeof(commands)));
    checkpoint_unlock();
    CHECK(!checkpoint_delete(&paths));
    CHECK(saved_round() > 0);
    checkpoint_lock();
    CHECK(checkpoint_clear(&paths, NULL, commands, sizeof(commands)));
    checkpoint_unlock();
    CHECK(checkpoint_delete(&paths));
    CHECK(access(checkpoint_path, F_OK) != 0 && saved_round() == -1);
    CHECK(checkpoint_delete(&paths)); // nothing to delete
    // an unreadable checkpoint.ini goes too
    write_text(checkpoint_path, "this isn't a checkpoint\n");
    CHECK(checkpoint_delete(&paths) && access(checkpoint_path, F_OK) != 0);
    // and so does a save the writer hasn't written yet
    checkpoint_autosave_init(&paths, 300, report);
    CHECK(post_and_wait(game(3, "pistol_m1911"), ""));
    CHECK(checkpoint_delete(&paths));
    int before = reports_now();
    CHECK(checkpoint_autosave_post(game(4, "pistol_m1911"), ""));
    CHECK(checkpoint_delete(&paths));
    checkpoint_autosave_flush();
    CHECK(reports_now() == before && access(checkpoint_path, F_OK) != 0);
    checkpoint_autosave_init(&paths, 0, report);
}

static void *try_from_another_thread(void *result) {
    *(bool *)result = checkpoint_trylock();
    if (*(bool *)result)
        checkpoint_unlock();
    return NULL;
}

static bool trylock_elsewhere(void) {
    pthread_t thread;
    bool result = false;
    pthread_create(&thread, NULL, try_from_another_thread, &result);
    pthread_join(thread, NULL);
    return result;
}

static void test_lock(void) {
    CHECK(trylock_elsewhere());
    checkpoint_lock();
    CHECK(!trylock_elsewhere());
    checkpoint_lock(); // recursive
    CHECK(checkpoint_trylock());
    checkpoint_unlock();
    checkpoint_unlock();
    CHECK(!trylock_elsewhere());
    checkpoint_unlock();
    CHECK(trylock_elsewhere());
}

// the UI's thread reading the checkpoint while the writer saves round after round
static void *post_rounds(void *context) {
    (void)context;
    for (long round = 100; round < 300; round++) {
        checkpoint_autosave_post(game(round, "ray_gun_upgraded,pistol_burst"), "stress");
        if (round % 8 == 0)
            usleep(200);
    }
    return NULL;
}

static void test_concurrent(void) {
    pthread_t thread;
    pthread_create(&thread, NULL, post_rounds, NULL);
    static char lines[16384];
    int unreadable = 0;
    for (int i = 0; i < 400; i++) {
        checkpoint_lock();
        checkpoint_read(&paths, lines, sizeof(lines));
        checkpoint_resume_state(&paths);
        checkpoint_unlock();
        if (lines[0] != '\0' && strstr(lines, "Round=") == NULL)
            unreadable++;
        // the timer's way: never waiting for the writer
        if (checkpoint_trylock())
            checkpoint_unlock();
    }
    pthread_join(thread, NULL);
    checkpoint_autosave_flush();
    CHECK(unreadable == 0);
    CHECK(saved_round() == 299);
}

int main(void) {
    if (mkdtemp(root) == NULL) {
        printf("FAIL: no temporary folder\n");
        return 1;
    }
    snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/checkpoint.ini", root);
    snprintf(players, sizeof(players), "%s/", root);
    snprintf(mutations, sizeof(mutations), "%s/AAECustomMutations_Default.cfg", root);
    paths.checkpoint = checkpoint_path;
    paths.players = players;
    test_may_replace();
    test_writer();
    test_resume();
    test_interval();
    test_delete();
    test_lock();
    test_concurrent();
    unlink(checkpoint_path);
    unlink(mutations);
    rmdir(root);
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    const char *sanitizer = " (thread sanitizer)";
#else
    const char *sanitizer = "";
#endif
#else
    const char *sanitizer = "";
#endif
    if (failures == 0)
        printf("test_checkpoint_autosave%s: all passed\n", sanitizer);
    return failures == 0 ? 0 : 1;
}
