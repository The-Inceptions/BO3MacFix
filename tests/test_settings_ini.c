// settings_ini.c against odd settings.ini files: spacing, quotes, CRLF, a byte order mark, empty values, long
// lines, duplicates, a missing or unreadable file.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../source/settings_ini.h"

static char dir[] = "/tmp/bo3mf_settings_ini_XXXXXX";
static char path[256];

static void put(const char *text) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fputs(text, fp);
    fclose(fp);
}

static char *get(void) {
    static char text[8192];
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    size_t n = fread(text, 1, sizeof(text) - 1, fp);
    text[n] = '\0';
    fclose(fp);
    return text;
}

static int read_int(const char *text, const char *name, int fallback) {
    put(text);
    return settings_ini_read(path, name, fallback, 10);
}

static int read_hex(const char *text, const char *name, int fallback) {
    put(text);
    return settings_ini_read(path, name, fallback, 16);
}

// the text read for name, or "(none)"
static const char *read_text(const char *text, const char *name, size_t size) {
    static char value[64];
    put(text);
    return settings_ini_read_string(path, name, value, size) ? value : "(none)";
}

static void test_reading_text(void) {
    const size_t big = 64;
    assert(strcmp(read_text("AAEBuildTime = Wed Dec 10 17:10:46 2025\n", "AAEBuildTime", big), "Wed Dec 10 17:10:46 2025") == 0);
    assert(strcmp(read_text("AAEBuildTime=Thu Mar  5 09:08:07 2026\r\n", "AAEBuildTime", big), "Thu Mar  5 09:08:07 2026") == 0);
    assert(strcmp(read_text("\xEF\xBB\xBF\tAAEBuildTime \t= \t\"Wed Dec 10 17:10:46 2025\" \t\n", "AAEBuildTime", big),
                  "Wed Dec 10 17:10:46 2025") == 0);
    assert(strcmp(read_text("AAEBuildTime = 'x y'", "AAEBuildTime", big), "x y") == 0);
    // a lone quote stays
    assert(strcmp(read_text("AAEBuildTime = \"x\n", "AAEBuildTime", big), "\"x") == 0);
    // the last line with text wins, an empty one doesn't count
    assert(strcmp(read_text("AAEBuildTime = a\nAAEBuildTime = b\nAAEBuildTime =\nAAEBuildTime = \"\"\n", "AAEBuildTime", big),
                  "b") == 0);
    // not set, a comment, another setting, too long for out
    assert(strcmp(read_text("// AAEBuildTime = a\nAAEBuildTimeX = b\nAAEBuildTime b\n", "AAEBuildTime", big), "(none)") == 0);
    assert(strcmp(read_text("AAEBuildTime = abcd\n", "AAEBuildTime", 4), "(none)") == 0);
    assert(strcmp(read_text("AAEBuildTime = abc\n", "AAEBuildTime", 4), "abc") == 0);
    // a line too long to read whole doesn't set it
    char long_line[1200];
    memset(long_line, 'x', sizeof(long_line));
    memcpy(long_line, "AAEBuildTime = ", 15);
    long_line[sizeof(long_line) - 1] = '\0';
    assert(strcmp(read_text(long_line, "AAEBuildTime", big), "(none)") == 0);
    unlink(path);
    assert(strcmp(read_text("", "AAEBuildTime", big), "(none)") == 0);
    unlink(path);
    char value[8] = "kept";
    assert(!settings_ini_read_string(path, "AAEBuildTime", value, sizeof(value)) && strcmp(value, "kept") == 0);
}

static void check_write(const char *before, const char *name, const char *value, const char *after) {
    if (before != NULL)
        put(before);
    else
        unlink(path);
    assert(settings_ini_write(path, name, value));
    if (strcmp(get(), after) != 0) {
        fprintf(stderr, "write %s = %s\n  got:  \"%s\"\n  want: \"%s\"\n", name, value, get(), after);
        exit(1);
    }
}

static void test_reading(void) {
    assert(read_int("JobThreads = 4\n", "JobThreads", 2) == 4);
    assert(read_int("  JobThreads=3\r\n", "JobThreads", 2) == 3);
    assert(read_int("\tJobThreads\t=\t4", "JobThreads", 2) == 4);
    assert(read_int("JobThreads = \"4\"\n", "JobThreads", 2) == 4);
    assert(read_int("JobThreads = '4'\n", "JobThreads", 2) == 4);
    assert(read_int("FrameStats = 5 ; every 5 seconds\n", "FrameStats", 0) == 5);
    assert(read_int("\xEF\xBB\xBFJobThreads = 4\n", "JobThreads", 2) == 4);
    // not this setting
    assert(read_int("JobThreadsX = 4\n", "JobThreads", 2) == 2);
    assert(read_int("Job = 4\n", "JobThreads", 2) == 2);
    assert(read_int("# JobThreads = 4\n; JobThreads = 4\n", "JobThreads", 2) == 2);
    assert(read_int("JobThreads 4\n", "JobThreads", 2) == 2);
    // no value or not a number: the default, not 0 (ExitFix = would otherwise turn ExitFix off)
    assert(read_int("ExitFix =\n", "ExitFix", 1) == 1);
    assert(read_int("ExitFix = \r\n", "ExitFix", 1) == 1);
    assert(read_int("ExitFix = on\n", "ExitFix", 1) == 1);
    assert(read_int("ExitFix = \"\"\n", "ExitFix", 1) == 1);
    assert(read_int("ExitFix = 0\n", "ExitFix", 1) == 0);
    assert(read_int("GyroSensitivity = -5\n", "GyroSensitivity", 100) == -5);
    // the last line with a number wins
    assert(read_int("JobThreads = 3\nJobThreads = 4\n", "JobThreads", 2) == 4);
    assert(read_int("JobThreads = 3\nJobThreads =\n", "JobThreads", 2) == 3);
    // colours
    assert(read_hex("LightbarColor = 0040FF\n", "LightbarColor", -1) == 0x0040FF);
    assert(read_hex("LightbarColor = #00ff00\n", "LightbarColor", -1) == 0x00FF00);
    assert(read_hex("LightbarColor = 0xFF0000\n", "LightbarColor", -1) == 0xFF0000);
    assert(read_hex("LightbarColor = \"#FFFFFF\"\n", "LightbarColor", -1) == 0xFFFFFF);
    assert(read_hex("LightbarColor = -1\n", "LightbarColor", 0) == -1);
    assert(read_hex("LightbarColor =\n", "LightbarColor", -1) == -1);
    // too big for an int
    assert(read_int("FrameStats = 99999999999\n", "FrameStats", 0) == 2147483647);
    // a long line is read in pieces, and a piece that starts with a name isn't a line that sets it
    char long_line[1200];
    memset(long_line, 'x', sizeof(long_line));
    long_line[0] = '#';
    strcpy(long_line + 1023, "JobThreads = 4\n");
    assert(read_int(long_line, "JobThreads", 2) == 2);
    // a missing file
    unlink(path);
    assert(settings_ini_read(path, "JobThreads", 2, 10) == 2);
}

static void test_writing(void) {
    // a new file
    check_write(NULL, "JobThreads", "4", "JobThreads = 4\n");
    check_write("", "JobThreads", "4", "JobThreads = 4\n");
    // the line is replaced where it is, comments and other lines stay as they are
    check_write("; BO3MacFix settings\nJobThreads = 2 ; default\n\n  # spare\nExitFix = 1\n", "JobThreads", "4",
                "; BO3MacFix settings\nJobThreads = 4\n\n  # spare\nExitFix = 1\n");
    // added at the end, after a last line without a newline
    check_write("ExitFix = 1", "JobThreads", "3", "ExitFix = 1\nJobThreads = 3\n");
    check_write("ExitFix = 1\n", "JobThreads", "3", "ExitFix = 1\nJobThreads = 3\n");
    // a last line without a newline that sets it
    check_write("ExitFix = 1\nJobThreads = 2", "JobThreads", "3", "ExitFix = 1\nJobThreads = 3\n");
    // duplicates go, the first one gets the value
    check_write("JobThreads = 2\nExitFix = 1\nJobThreads = 3\n", "JobThreads", "4", "JobThreads = 4\nExitFix = 1\n");
    // names that only start the same stay
    check_write("JobThreadsX = 1\n", "JobThreads", "4", "JobThreadsX = 1\nJobThreads = 4\n");
    // CRLF files get CRLF lines
    check_write("ExitFix = 1\r\nJobThreads = 2\r\n", "JobThreads", "4", "ExitFix = 1\r\nJobThreads = 4\r\n");
    check_write("ExitFix = 1\r\n", "JobThreads", "4", "ExitFix = 1\r\nJobThreads = 4\r\n");
    // with a byte order mark
    check_write("\xEF\xBB\xBFJobThreads = 2\n", "JobThreads", "4", "JobThreads = 4\n");

    // long lines stay whole, and a long line that sets it is replaced whole
    char before[3000], after[3000];
    memset(before, 'x', 2000);
    before[0] = '#';
    strcpy(before + 2000, "\nExitFix = 1\n");
    strcpy(after, before);
    strcat(after, "JobThreads = 4\n");
    check_write(before, "JobThreads", "4", after);
    strcpy(before, "JobThreads = 2 ;");
    memset(before + 16, 'x', 2000);
    strcpy(before + 2016, "\nExitFix = 1\n");
    check_write(before, "JobThreads", "4", "JobThreads = 4\nExitFix = 1\n");

    // no temporary file is left behind
    char temp[300];
    snprintf(temp, sizeof(temp), "%s.tmp%d", path, (int)getpid());
    assert(access(temp, F_OK) != 0);

    // a file that can't be read is left alone (skipped when running as root, who can read it anyway)
    put("ExitFix = 1\n");
    chmod(path, 0);
    FILE *readable = fopen(path, "r");
    if (readable == NULL) {
        assert(!settings_ini_write(path, "JobThreads", "4"));
        chmod(path, 0644);
        assert(strcmp(get(), "ExitFix = 1\n") == 0);
    } else {
        fclose(readable);
        chmod(path, 0644);
    }
    // a folder that can't be written to: the file is left alone
    put("ExitFix = 1\n");
    chmod(dir, 0555);
    if (access(dir, W_OK) != 0) {
        assert(!settings_ini_write(path, "JobThreads", "4"));
        assert(strcmp(get(), "ExitFix = 1\n") == 0);
    }
    chmod(dir, 0755);
}

int main(void) {
    assert(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/settings.ini", dir);
    test_reading();
    test_reading_text();
    test_writing();
    unlink(path);
    rmdir(dir);
    puts("settings.ini tests passed");
    return 0;
}
