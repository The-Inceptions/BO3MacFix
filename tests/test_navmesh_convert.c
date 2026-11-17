// navmesh_convert.c: which map names the converter runs for, and that its arguments reach it as they are, with no
// shell reading them, against a made-up wine (a script in a folder of the test's own, under /tmp).
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../source/navmesh_convert.h"

extern char **environ;

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char dir[] = "/tmp/bo3macfix-navmesh-test-XXXXXX";

static void write_script(const char *name, const char *body) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        printf("FAIL: can't write %s\n", path);
        exit(1);
    }
    fprintf(file, "#!/bin/sh\n%s", body);
    fclose(file);
    chmod(path, 0755);
}

static bool exists(const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return access(path, F_OK) == 0;
}

static void test_names(void) {
    CHECK(navmesh_map_name_ok("zm_theater"));
    CHECK(navmesh_map_name_ok("cp_mi_cairo_infection2"));
    CHECK(navmesh_map_name_ok("zm_Leviathan-2"));
    CHECK(navmesh_map_name_ok("a"));
    char longest[64];
    memset(longest, 'z', 63);
    longest[63] = '\0';
    CHECK(navmesh_map_name_ok(longest));
    char too_long[65];
    memset(too_long, 'z', 64);
    too_long[64] = '\0';
    CHECK(!navmesh_map_name_ok(too_long));
    CHECK(!navmesh_map_name_ok(NULL));
    CHECK(!navmesh_map_name_ok(""));
    // what a shell would read as a command, or a path would read as a folder
    static const char *const refused[] = { "zm_a$(touch x)", "zm_`id`", "zm_\"x", "zm_'x", "zm_a;id", "zm_a&id", "zm a",
                                           "zm_a/../b", "zm_a\\b", "..", "zm.a", "zm_a$HOME", "zm_\xc3\xa9", "zm_a\n" };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (navmesh_map_name_ok(refused[i])) {
            printf("FAIL: the map name \"%s\" is taken\n", refused[i]);
            failures++;
        }
    }
}

static void test_arguments_as_they_are(void) {
    // wine prints its arguments one per line, then says how many there were
    write_script("wine", "for a in \"$@\"; do printf '[%s]\\n' \"$a\"; done\necho \"$#\"\n");
    char wine[PATH_MAX], pwned_dollar[PATH_MAX], pwned_quote[PATH_MAX];
    snprintf(wine, sizeof(wine), "%s/wine", dir);
    // a game folder with every character sh would act on, and a command in each of the three ways sh runs one
    snprintf(pwned_dollar, sizeof(pwned_dollar), "Z:/Games/My \"BO3\" $HOME `touch %s/backquote` $(touch %s/dollar)",
             dir, dir);
    snprintf(pwned_quote, sizeof(pwned_quote), "Z:/x\"; touch %s/quote; echo \"", dir);
    char output[1024];
    int status = -1;
    CHECK(navmesh_convert_run(wine, "../../../BO3MacFix/cod2map/cod2map64.exe", pwned_dollar, pwned_quote, output,
                              sizeof(output), &status));
    CHECK(status == 0);
    char expected[4096];
    snprintf(expected, sizeof(expected), "[../../../BO3MacFix/cod2map/cod2map64.exe]\n[-mac_navmesh]\n[%s]\n[%s]\n4\n",
             pwned_dollar, pwned_quote);
    CHECK(strcmp(output, expected) == 0);
    if (strcmp(output, expected) != 0)
        printf("  got:\n%s  expected:\n%s", output, expected);
    // nothing in them ran
    CHECK(!exists("backquote"));
    CHECK(!exists("dollar"));
    CHECK(!exists("quote"));
}

static void test_output(void) {
    // more output than the buffer holds: the first part is kept, and wine isn't stopped by a full pipe
    write_script("chatty", "i=0\nwhile [ $i -lt 400 ]; do echo 0123456789012345678901234567890123456789; i=$((i+1)); done\n"
                           "touch \"$(dirname \"$0\")/finished\"\nexit 7\n");
    char chatty[PATH_MAX];
    snprintf(chatty, sizeof(chatty), "%s/chatty", dir);
    char output[64];
    int status = -1;
    CHECK(navmesh_convert_run(chatty, "c", "a", "b", output, sizeof(output), &status));
    CHECK(status == 7);
    CHECK(strlen(output) == sizeof(output) - 1);
    static const char start[] = "0123456789012345678901234567890123456789\n0123";
    CHECK(strncmp(output, start, sizeof(start) - 1) == 0);
    CHECK(exists("finished"));
    // no room for output at all
    status = -1;
    CHECK(navmesh_convert_run(chatty, "c", "a", "b", NULL, 0, &status));
    CHECK(status == 7);
    // a wine that isn't there
    status = 5;
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/no-such-wine", dir);
    CHECK(!navmesh_convert_run(missing, "c", "a", "b", output, sizeof(output), &status));
    CHECK(status == -1);
    CHECK(output[0] == '\0');
    // a wine named without its folder is looked for in PATH, as sh did
    const char *path = getenv("PATH");
    char *saved = strdup(path != NULL ? path : "");
    setenv("PATH", dir, 1);
    status = -1;
    CHECK(navmesh_convert_run("wine", "c", "a", "b", output, sizeof(output), &status));
    CHECK(status == 0 && strncmp(output, "[c]\n", 4) == 0);
    setenv("PATH", saved, 1);
    free(saved);
}

// The environment: what /bin/sh gave Wine, without any DYLD_ variable. Shown first on the function, then on a wine
// that is a compiled program (this test itself, started with --fake-wine): a shell script couldn't show it, since
// sh starts without DYLD_ variables whatever it is given.
static void test_environment(void) {
    char *from[] = { "PATH=/usr/bin:/bin", "DYLD_INSERT_LIBRARIES=/x/quietcrash.dylib", "HOME=/Users/x",
                     "DYLD_LIBRARY_PATH=/x/lib", "DYLD_FRAMEWORK_PATH=/x/f", "DYLD_FALLBACK_LIBRARY_PATH=/y",
                     "DYLD_PRINT_LIBRARIES=1", "XDYLD_LIBRARY_PATH=kept", "DYLD=kept", "dyld_lower=kept",
                     "WINEPREFIX=/Games/BO3/BO3MacFix/pfx", NULL };
    char **kept = navmesh_environment(from);
    CHECK(kept != NULL);
    if (kept != NULL) {
        static const char *const wanted[] = { "PATH=/usr/bin:/bin", "HOME=/Users/x", "XDYLD_LIBRARY_PATH=kept",
                                              "DYLD=kept", "dyld_lower=kept", "WINEPREFIX=/Games/BO3/BO3MacFix/pfx" };
        size_t i = 0;
        for (; kept[i] != NULL && i < sizeof(wanted) / sizeof(wanted[0]); i++)
            CHECK(strcmp(kept[i], wanted[i]) == 0);
        CHECK(i == sizeof(wanted) / sizeof(wanted[0]) && kept[i] == NULL);
        free(kept);
    }
    char *none[] = { NULL };
    kept = navmesh_environment(none);
    CHECK(kept != NULL && kept[0] == NULL);
    free(kept);
    kept = navmesh_environment(NULL);
    CHECK(kept != NULL && kept[0] == NULL);
    free(kept);

    // the real thing: the game's environment with DYLD_ variables that are harmless to a starting program
    char self[PATH_MAX];
    uint32_t size = sizeof(self);
    if (_NSGetExecutablePath(self, &size) != 0) {
        printf("FAIL: no path of the test itself\n");
        failures++;
        return;
    }
    setenv("DYLD_LIBRARY_PATH", "/nonexistent-bo3macfix-test", 1);
    setenv("DYLD_FRAMEWORK_PATH", "/nonexistent-bo3macfix-test", 1);
    setenv("DYLD_BO3MACFIX_TEST", "1", 1);
    setenv("BO3MACFIX_NAVMESH_TEST", "kept", 1);
    char output[2048];
    int status = -1;
    CHECK(navmesh_convert_run(self, "--fake-wine", "a", "b", output, sizeof(output), &status));
    CHECK(status == 0);
    CHECK(strstr(output, "BO3MACFIX_NAVMESH_TEST=kept\n") != NULL); // the rest of the environment arrives
    CHECK(strstr(output, "DYLD_") == NULL);                         // and no DYLD_ variable
    if (strstr(output, "DYLD_") != NULL)
        printf("  wine got: %s", output);
    unsetenv("DYLD_LIBRARY_PATH");
    unsetenv("DYLD_FRAMEWORK_PATH");
    unsetenv("DYLD_BO3MACFIX_TEST");
    unsetenv("BO3MACFIX_NAVMESH_TEST");
}

// A converter that leaves a process of its own behind holding the pipe (as Wine's background processes can): its
// output and exit status come when it ends, not when the one left behind lets the pipe go (pclose didn't wait for it).
static void test_left_behind(void) {
    write_script("leaves", "(sleep 3; echo late) &\necho done\nexit 4\n");
    char leaves[PATH_MAX];
    snprintf(leaves, sizeof(leaves), "%s/leaves", dir);
    char output[256];
    int status = -1;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(navmesh_convert_run(leaves, "c", "a", "b", output, sizeof(output), &status));
    clock_gettime(CLOCK_MONOTONIC, &end);
    double seconds = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    CHECK(status == 4);
    CHECK(strcmp(output, "done\n") == 0);
    CHECK(seconds < 2.0);
    if (seconds >= 2.0)
        printf("  it waited %.1f s for the process left behind\n", seconds);
}

int main(int argc, char **argv) {
    // started by test_environment as the made-up wine: says which variables it got
    if (argc > 1 && strcmp(argv[1], "--fake-wine") == 0) {
        for (char **variable = environ; *variable != NULL; variable++)
            if (strncmp(*variable, "DYLD_", 5) == 0 || strncmp(*variable, "BO3MACFIX_NAVMESH_TEST=", 23) == 0)
                printf("%s\n", *variable);
        return 0;
    }
    if (mkdtemp(dir) == NULL) {
        printf("FAIL: no folder for the test\n");
        return 1;
    }
    test_names();
    test_arguments_as_they_are();
    test_output();
    test_environment();
    test_left_behind();
    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    if (system(command) != 0)
        printf("note: %s is left behind\n", dir);
    if (failures != 0) {
        printf("test_navmesh_convert: %d failed\n", failures);
        return 1;
    }
    printf("test_navmesh_convert: all passed\n");
    return 0;
}
