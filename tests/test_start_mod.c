// Tests for StartWithMod: start_mod.c (which names may pass, lastmod.txt and its marker, the mod's files, the
// launcher's decisions, a workshop item's title, the notices) and start_mod_game.c (what the game does with the
// launcher's note and at its first menus, against a made-up loaded mod's name), with made-up game, workshop and
// BO3MacFix folders in a temporary folder of its own.
#define _DARWIN_C_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../source/launch_args.h"
#include "../source/offsets.h"
#include "../source/start_mod.h"
#include "../source/start_mod_game.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define ARGS(...) (int)(sizeof((char *[]){ __VA_ARGS__ }) / sizeof(char *)), (char *[]){ __VA_ARGS__ }
#define LAUNCHER "/Game/CoDBlkOps3.app/Contents/MacOS/AppBundleExe"
#define AAE "2631943123"

static char root[PATH_MAX];
static char folder[PATH_MAX], game[PATH_MAX], workshop[PATH_MAX], memory[PATH_MAX], marker[PATH_MAX];
static start_mod_paths_t paths;

// the notices the game posted (notices.m's in the game)
static char posted[8][256];
static int posted_count;
void notices_post_lasting(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (posted_count < 8)
        vsnprintf(posted[posted_count++], sizeof(posted[0]), format, args);
    va_end(args);
}

static void path_of(char *out, const char *a, const char *b) {
    snprintf(out, PATH_MAX, "%s/%s", a, b);
}

static void make_folder(const char *path) {
    char partial[PATH_MAX];
    snprintf(partial, sizeof(partial), "%s", path);
    for (char *p = partial + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(partial, 0755);
            *p = '/';
        }
    }
    if (mkdir(partial, 0755) != 0 && errno != EEXIST) {
        printf("FAIL: couldn't make %s\n", path);
        exit(1);
    }
}

static void write_bytes(const char *path, const char *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        printf("FAIL: couldn't write %s\n", path);
        exit(1);
    }
    fwrite(bytes, 1, length, file);
    fclose(file);
}

static void write_text(const char *path, const char *text) {
    write_bytes(path, text, strlen(text));
}

static const char *read_text(const char *path) {
    static char text[4096];
    text[0] = '\0';
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    text[length] = '\0';
    fclose(file);
    return text;
}

static bool exists(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0;
}

static int remove_entry(const char *path, const struct stat *info, int flag, struct FTW *walk) {
    (void)info, (void)flag, (void)walk;
    return remove(path);
}

static void remove_tree(const char *path) {
    nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

// how many files of folder have a name that starts with prefix (left-over temporary files)
static int count_starting(const char *dir, const char *prefix) {
    DIR *listing = opendir(dir);
    if (listing == NULL)
        return -1;
    int count = 0;
    for (struct dirent *entry = readdir(listing); entry != NULL; entry = readdir(listing))
        count += strncmp(entry->d_name, prefix, strlen(prefix)) == 0;
    closedir(listing);
    return count;
}

// a fresh made-up Steam library: the game's folder with BO3MacFix's, the workshop with All-around Enhancement. The
// test works in the game's MacOS folder, as the game does, so the game's relative paths stay inside it.
static void fresh_folders(void) {
    char steamapps[PATH_MAX], item[PATH_MAX], zone[PATH_MAX], macos[PATH_MAX];
    path_of(steamapps, root, "steamapps");
    if (chdir(root) != 0) {
        printf("FAIL: couldn't go to %s\n", root);
        exit(1);
    }
    remove_tree(steamapps);
    snprintf(game, sizeof(game), "%s/common/Call of Duty Black Ops III", steamapps);
    path_of(macos, game, "CoDBlkOps3.app/Contents/MacOS");
    make_folder(macos);
    if (chdir(macos) != 0) {
        printf("FAIL: couldn't go to %s\n", macos);
        exit(1);
    }
    path_of(folder, game, "BO3MacFix");
    path_of(workshop, steamapps, "workshop");
    path_of(memory, folder, Start_Mod_Memory_File);
    path_of(marker, folder, Start_Mod_Marker_File);
    make_folder(folder);
    snprintf(item, sizeof(item), "%s/content/311210/" AAE, workshop);
    make_folder(item);
    path_of(zone, item, "core_mod.ff");
    write_text(zone, "TAff0100");
    path_of(zone, item, "workshop.json");
    write_text(zone, "{\n    \"Description\": \"say \\\"Title\\\": \\\"no\\\"\",\n"
                     "    \"FolderName\": \"all_around_enhancement\",\n"
                     "    \"Title\": \"^1[ZM] ^3All-around Enhancement v4.1.7\",\n    \"Type\": \"mod\"\n}\n");
    paths.folder = folder;
    paths.game = game;
    paths.workshop = workshop;
}

// what stdout gets while a call runs (the game's log lines)
static int saved_stdout = -1;
static char captured[8192];
static void capture_start(void) {
    char path[PATH_MAX];
    path_of(path, root, "stdout.txt");
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, STDOUT_FILENO);
    close(fd);
}
static const char *capture_end(void) {
    char path[PATH_MAX];
    path_of(path, root, "stdout.txt");
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    const char *text = read_text(path);
    snprintf(captured, sizeof(captured), "%s", text != NULL ? text : "");
    return captured;
}

static void test_names(void) {
    // what fs_game holds for a workshop mod and for a mod of the game's own folder, in 1 to 31 characters
    CHECK(start_mod_name_ok(AAE));
    CHECK(start_mod_name_ok("zm_mymod"));
    CHECK(start_mod_name_ok("My-Mod.v2"));
    CHECK(start_mod_name_ok("a"));
    CHECK(start_mod_name_ok("x234567890123456789012345678901")); // 31
    CHECK(!start_mod_name_ok("x2345678901234567890123456789012")); // 32
    // nothing a command line or a path would read as more
    CHECK(!start_mod_name_ok(NULL));
    CHECK(!start_mod_name_ok(""));
    CHECK(!start_mod_name_ok("my mod"));
    CHECK(!start_mod_name_ok("x;quit"));
    CHECK(!start_mod_name_ok("x+map"));
    CHECK(!start_mod_name_ok("x\"y"));
    CHECK(!start_mod_name_ok("a/b"));
    CHECK(!start_mod_name_ok("a\\b"));
    CHECK(!start_mod_name_ok("a..b"));
    CHECK(!start_mod_name_ok(".."));
    CHECK(!start_mod_name_ok(".hidden"));
    CHECK(!start_mod_name_ok("-AspyrExe=x"));
    CHECK(!start_mod_name_ok("-mymod"));
    CHECK(start_mod_name_ok("my-mod-"));
    CHECK(!start_mod_name_ok("x\ny"));
    CHECK(!start_mod_name_ok("x\ty"));
    CHECK(!start_mod_name_ok("caf\xc3\xa9"));
    CHECK(!start_mod_name_ok("a:b"));
    CHECK(!start_mod_name_ok("a%s"));
    // the stand-in for custom maps, in any case
    CHECK(!start_mod_name_ok("usermaps"));
    CHECK(!start_mod_name_ok("UserMaps"));
    CHECK(start_mod_is_stand_in("USERMAPS"));
    CHECK(start_mod_is_stand_in("usermaps"));
    CHECK(!start_mod_is_stand_in("usermaps2"));
    CHECK(!start_mod_is_stand_in(NULL));
    CHECK(start_mod_name_ok("usermaps2"));
}

static start_mod_loaded_t loaded_of(const char *bytes, size_t length, char *name) {
    char raw[Start_Mod_Name_Size];
    memset(raw, 0, sizeof(raw));
    memcpy(raw, bytes, length);
    return start_mod_loaded(raw, name);
}

static void test_loaded(void) {
    char name[Start_Mod_Name_Size];
    CHECK(loaded_of("", 0, name) == Start_Mod_Loaded_None && name[0] == '\0');
    CHECK(loaded_of(AAE, 10, name) == Start_Mod_Loaded_Mod && strcmp(name, AAE) == 0);
    // the name in the game's case (the mods list's entry): remembered as it is
    CHECK(loaded_of("MyMod", 5, name) == Start_Mod_Loaded_Mod && strcmp(name, "MyMod") == 0);
    CHECK(loaded_of("usermaps", 8, name) == Start_Mod_Loaded_Stand_In && strcmp(name, "usermaps") == 0);
    CHECK(loaded_of("my mod", 6, name) == Start_Mod_Loaded_Unfit && strcmp(name, "my mod") == 0);
    // what isn't printable goes to the log as '?'
    CHECK(loaded_of("a\x01\xff", 3, name) == Start_Mod_Loaded_Unfit && strcmp(name, "a??") == 0);
    // 31 characters and the game's 0
    CHECK(loaded_of("x234567890123456789012345678901", 31, name) == Start_Mod_Loaded_Mod && strlen(name) == 31);
    // 0x20 bytes without a 0 are no name the game could have loaded
    char full[Start_Mod_Name_Size];
    memset(full, 'a', sizeof(full));
    CHECK(start_mod_loaded(full, name) == Start_Mod_Loaded_Unfit && strlen(name) == 31);
    // what comes after the game's 0 doesn't count
    char tail[Start_Mod_Name_Size];
    memset(tail, 'z', sizeof(tail));
    memcpy(tail, "abc\0", 4);
    CHECK(start_mod_loaded(tail, name) == Start_Mod_Loaded_Mod && strcmp(name, "abc") == 0);
}

static start_mod_file_t parse_of(const char *text, size_t length, char *name) {
    return start_mod_parse(text, length, name);
}
#define PARSE(text, name) parse_of(text, sizeof(text) - 1, name)

static void test_parse(void) {
    char name[Start_Mod_Name_Size];
    CHECK(PARSE("", name) == Start_Mod_File_Absent);
    CHECK(PARSE("\n", name) == Start_Mod_File_Absent);
    CHECK(PARSE("\r\n", name) == Start_Mod_File_Absent);
    CHECK(PARSE(AAE "\n", name) == Start_Mod_File_Named && strcmp(name, AAE) == 0);
    CHECK(PARSE(AAE, name) == Start_Mod_File_Named && strcmp(name, AAE) == 0);
    CHECK(PARSE(AAE "\r\n", name) == Start_Mod_File_Named && strcmp(name, AAE) == 0);
    CHECK(PARSE("x234567890123456789012345678901\n", name) == Start_Mod_File_Named);
    // anything else is none of ours, and holds no mod
    CHECK(PARSE("x2345678901234567890123456789012\n", name) == Start_Mod_File_Damaged && name[0] == '\0');
    CHECK(PARSE(AAE "\n" AAE "\n", name) == Start_Mod_File_Damaged && name[0] == '\0');
    CHECK(PARSE(AAE "\n\n", name) == Start_Mod_File_Damaged);
    CHECK(PARSE(AAE " \n", name) == Start_Mod_File_Damaged);
    CHECK(PARSE(" " AAE "\n", name) == Start_Mod_File_Damaged);
    CHECK(PARSE("263194\0" "3123\n", name) == Start_Mod_File_Damaged);
    CHECK(PARSE("usermaps\n", name) == Start_Mod_File_Damaged);
    CHECK(PARSE("x;quit\n", name) == Start_Mod_File_Damaged);
    CHECK(PARSE("\r", name) == Start_Mod_File_Damaged);
    CHECK(PARSE("\xef\xbb\xbf" AAE "\n", name) == Start_Mod_File_Damaged);
}

static void test_read_write(void) {
    char name[Start_Mod_Name_Size];
    fresh_folders();
    CHECK(start_mod_read(memory, name) == Start_Mod_File_Absent && name[0] == '\0');
    CHECK(start_mod_write(memory, AAE));
    CHECK(strcmp(read_text(memory), AAE "\n") == 0);
    CHECK(start_mod_read(memory, name) == Start_Mod_File_Named && strcmp(name, AAE) == 0);
    // replaced whole, no temporary file left behind
    CHECK(start_mod_write(memory, "zm_mymod"));
    CHECK(strcmp(read_text(memory), "zm_mymod\n") == 0);
    CHECK(count_starting(folder, "lastmod.txt.tmp") == 0);
    // "" removes it, also when it isn't there
    CHECK(start_mod_write(memory, ""));
    CHECK(!exists(memory));
    CHECK(start_mod_write(memory, ""));
    CHECK(start_mod_write(memory, NULL));
    // a name that may not be remembered isn't written, and the file stays as it was
    CHECK(start_mod_write(memory, AAE));
    CHECK(!start_mod_write(memory, "usermaps"));
    CHECK(!start_mod_write(memory, "x;quit"));
    CHECK(strcmp(read_text(memory), AAE "\n") == 0);
    // a huge file, binary junk, a folder or a link in its place: damaged
    char huge[100000];
    memset(huge, '7', sizeof(huge));
    write_bytes(memory, huge, sizeof(huge));
    CHECK(start_mod_read(memory, name) == Start_Mod_File_Damaged && name[0] == '\0');
    write_bytes(memory, "x23456789012345678901234567890123456789012345678901234567890123", 63);
    CHECK(start_mod_read(memory, name) == Start_Mod_File_Damaged);
    write_bytes(memory, "\x00\x01\xfe\xff", 4);
    CHECK(start_mod_read(memory, name) == Start_Mod_File_Damaged);
    CHECK(start_mod_write(memory, AAE)); // replaces a damaged one
    CHECK(strcmp(read_text(memory), AAE "\n") == 0);
    unlink(memory);
    make_folder(memory);
    CHECK(start_mod_read(memory, name) == Start_Mod_File_Damaged);
    CHECK(!start_mod_write(memory, AAE));
    CHECK(count_starting(folder, "lastmod.txt.tmp") == 0);
    rmdir(memory);
    char elsewhere[PATH_MAX];
    path_of(elsewhere, root, "elsewhere.txt");
    write_text(elsewhere, AAE "\n");
    CHECK(symlink(elsewhere, memory) == 0);
    CHECK(start_mod_read(memory, name) == Start_Mod_File_Damaged);
    unlink(memory);
    // a folder that can't be written to: false, nothing left
    chmod(folder, 0555);
    CHECK(!start_mod_write(memory, AAE));
    chmod(folder, 0755);
    CHECK(!exists(memory));
}

static void test_files(void) {
    char path[PATH_MAX], zone[PATH_MAX];
    fresh_folders();
    CHECK(start_mod_files(&paths, AAE) == Start_Mod_Files_There);
    // a workshop item without its zone file, or an empty one, or a folder in its place
    CHECK(start_mod_files(&paths, "1234567890") == Start_Mod_Files_Missing);
    snprintf(zone, sizeof(zone), "%s/content/311210/" AAE "/core_mod.ff", workshop);
    write_text(zone, "");
    CHECK(start_mod_files(&paths, AAE) == Start_Mod_Files_Missing);
    unlink(zone);
    make_folder(zone);
    CHECK(start_mod_files(&paths, AAE) == Start_Mod_Files_Missing);
    rmdir(zone);
    write_text(zone, "TAff");
    // Steam downloading it
    snprintf(path, sizeof(path), "%s/downloads/311210/" AAE, workshop);
    make_folder(path);
    CHECK(start_mod_files(&paths, AAE) == Start_Mod_Files_Updating);
    // another item's download doesn't count
    rmdir(path);
    snprintf(path, sizeof(path), "%s/downloads/311210/1111", workshop);
    make_folder(path);
    CHECK(start_mod_files(&paths, AAE) == Start_Mod_Files_There);
    // a mod of the game's own folder: mods/<name>/zone/core_mod.ff
    CHECK(start_mod_files(&paths, "zm_mymod") == Start_Mod_Files_Missing);
    snprintf(path, sizeof(path), "%s/mods/zm_mymod/zone", game);
    make_folder(path);
    CHECK(start_mod_files(&paths, "zm_mymod") == Start_Mod_Files_Missing);
    path_of(zone, path, "core_mod.ff");
    write_text(zone, "TAff");
    CHECK(start_mod_files(&paths, "zm_mymod") == Start_Mod_Files_There);
    // a name of digits that is a folder of the game's own
    snprintf(path, sizeof(path), "%s/mods/4242/zone", game);
    make_folder(path);
    path_of(zone, path, "core_mod.ff");
    write_text(zone, "TAff");
    CHECK(start_mod_files(&paths, "4242") == Start_Mod_Files_There);
    // a name that isn't one isn't looked for, even where it would lead to a mod's files
    CHECK(start_mod_files(&paths, "../BO3MacFix") == Start_Mod_Files_Missing);
    CHECK(start_mod_files(&paths, "../mods/zm_mymod") == Start_Mod_Files_Missing);
    // only a name of digits is a workshop item
    snprintf(path, sizeof(path), "%s/content/311210/zm_fake", workshop);
    make_folder(path);
    path_of(zone, path, "core_mod.ff");
    write_text(zone, "TAff");
    CHECK(start_mod_files(&paths, "zm_fake") == Start_Mod_Files_Missing);
    // no workshop or game folder given
    start_mod_paths_t bare = { folder, NULL, NULL };
    CHECK(start_mod_files(&bare, AAE) == Start_Mod_Files_Missing);
}

static start_mod_plan_t launch(bool enabled, int argc, char *const argv[], bool write, char *mod) {
    return start_mod_launch(&paths, enabled, argc, argv, write, mod);
}

static void test_launch(void) {
    char mod[Start_Mod_Name_Size], name[Start_Mod_Name_Size];
    fresh_folders();
    // nothing remembered yet
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Nothing && mod[0] == '\0');
    CHECK(!exists(marker));
    // remembered: given, with its marker
    CHECK(start_mod_write(memory, AAE));
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Given && strcmp(mod, AAE) == 0);
    CHECK(start_mod_read(marker, name) == Start_Mod_File_Named && strcmp(name, AAE) == 0);
    CHECK(strcmp(read_text(memory), AAE "\n") == 0);
    // the next start finds the marker: that start never got to the menus. Stock, the mod forgotten, the marker gone
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Failed && strcmp(mod, AAE) == 0);
    CHECK(!exists(marker) && !exists(memory));
    // and the start after it has nothing to give
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Nothing);
    // a marker counts whatever it holds, even with StartWithMod's memory gone or another mod in it
    CHECK(start_mod_write(memory, "zm_other"));
    write_text(marker, "junk; not a name\n");
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Failed && mod[0] == '\0');
    CHECK(!exists(marker) && !exists(memory));
    write_text(marker, "");
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Failed && mod[0] == '\0');
    CHECK(!exists(marker));
    // with write false (a dry run without a folder of its own) the files stay as they are
    CHECK(start_mod_write(memory, AAE));
    write_text(marker, AAE "\n");
    CHECK(launch(true, ARGS(LAUNCHER), false, mod) == Start_Mod_Failed && strcmp(mod, AAE) == 0);
    CHECK(exists(marker) && exists(memory));
    unlink(marker);
    CHECK(launch(true, ARGS(LAUNCHER), false, mod) == Start_Mod_Given && strcmp(mod, AAE) == 0);
    CHECK(!exists(marker));
    // off: nothing read, nothing written, the marker of an earlier start left for the game to remove
    write_text(marker, AAE "\n");
    CHECK(launch(false, ARGS(LAUNCHER), true, mod) == Start_Mod_Off && mod[0] == '\0');
    CHECK(exists(marker) && exists(memory));
    unlink(marker);
    // a damaged memory file
    write_text(memory, "2631943123 extra\n");
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Damaged && mod[0] == '\0');
    CHECK(!exists(marker));
    CHECK(start_mod_write(memory, AAE));
    // the command line's own fs_game or a map, a server, a lobby: nothing added, no marker, the mod still remembered
    CHECK(launch(true, ARGS(LAUNCHER, "+set", "fs_game", "1234"), true, mod) == Start_Mod_Busy &&
          strcmp(mod, AAE) == 0);
    CHECK(launch(true, ARGS(LAUNCHER, "+set fs_game usermaps"), true, mod) == Start_Mod_Busy);
    CHECK(launch(true, ARGS(LAUNCHER, "+devmap", "zm_zod"), true, mod) == Start_Mod_Busy);
    CHECK(launch(true, ARGS(LAUNCHER, "+map", "zm_theater"), true, mod) == Start_Mod_Busy);
    CHECK(launch(true, ARGS(LAUNCHER, "+connect_lobby", "109775241"), true, mod) == Start_Mod_Busy);
    CHECK(!exists(marker) && exists(memory));
    // other arguments don't stop it
    CHECK(launch(true, ARGS(LAUNCHER, "+set", "r_fullscreen", "1"), true, mod) == Start_Mod_Given);
    unlink(marker);
    // the mod's files: gone, or being downloaded
    char zone[PATH_MAX], download[PATH_MAX];
    snprintf(zone, sizeof(zone), "%s/content/311210/" AAE "/core_mod.ff", workshop);
    unlink(zone);
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Missing && strcmp(mod, AAE) == 0);
    CHECK(!exists(marker) && exists(memory));
    write_text(zone, "TAff");
    snprintf(download, sizeof(download), "%s/downloads/311210/" AAE, workshop);
    make_folder(download);
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Updating && strcmp(mod, AAE) == 0);
    CHECK(!exists(marker));
    rmdir(download);
    // a marker that can't be written: no start with the mod, since a failed one couldn't be told apart
    chmod(folder, 0555);
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Unsafe && strcmp(mod, AAE) == 0);
    chmod(folder, 0755);
    CHECK(!exists(marker));
    // a start that couldn't be made: the marker goes again
    CHECK(launch(true, ARGS(LAUNCHER), true, mod) == Start_Mod_Given && exists(marker));
    CHECK(start_mod_launch_abandoned(&paths) && !exists(marker));
    CHECK(start_mod_launch_abandoned(&paths));
    // no folder at all
    start_mod_paths_t none = { NULL, game, workshop };
    CHECK(start_mod_launch(&none, true, ARGS(LAUNCHER), true, mod) == Start_Mod_Unsafe);
    CHECK(!start_mod_launch_abandoned(&none));
}

static void test_notes(void) {
    char note[64], mod[Start_Mod_Name_Size];
    start_mod_plan_t plan;
    static const start_mod_plan_t all[] = { Start_Mod_Off, Start_Mod_Nothing, Start_Mod_Damaged, Start_Mod_Failed,
                                            Start_Mod_Busy, Start_Mod_Missing, Start_Mod_Updating, Start_Mod_Unsafe,
                                            Start_Mod_Given };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        start_mod_note(all[i], AAE, note, sizeof(note));
        CHECK(start_mod_parse_note(note, &plan, mod) && plan == all[i] && strcmp(mod, AAE) == 0);
        CHECK(strcmp(start_mod_plan_text(all[i]), "?") != 0);
        // the words the game reads
        CHECK(strchr(note, ' ') != NULL);
    }
    start_mod_note(Start_Mod_Given, AAE, note, sizeof(note));
    CHECK(strcmp(note, "given " AAE) == 0);
    start_mod_note(Start_Mod_Failed, "", note, sizeof(note));
    CHECK(strcmp(note, "failed ") == 0);
    CHECK(start_mod_parse_note(note, &plan, mod) && plan == Start_Mod_Failed && mod[0] == '\0');
    // a name that isn't one never goes into a note
    start_mod_note(Start_Mod_Given, "x;quit", note, sizeof(note));
    CHECK(strcmp(note, "given ") == 0);
    CHECK(!start_mod_parse_note(note, &plan, mod));
    // what the game doesn't take
    CHECK(!start_mod_parse_note(NULL, &plan, mod));
    CHECK(!start_mod_parse_note("", &plan, mod));
    CHECK(!start_mod_parse_note("given", &plan, mod));
    CHECK(!start_mod_parse_note("given x y", &plan, mod));
    CHECK(!start_mod_parse_note("given ;quit", &plan, mod));
    CHECK(!start_mod_parse_note("gift " AAE, &plan, mod));
    CHECK(!start_mod_parse_note("givenx " AAE, &plan, mod));
    CHECK(!start_mod_parse_note("give " AAE, &plan, mod));
    CHECK(!start_mod_parse_note(" given " AAE, &plan, mod));
    CHECK(!start_mod_parse_note("given usermaps", &plan, mod));
    CHECK(start_mod_parse_note("updating " AAE, &plan, mod) && plan == Start_Mod_Updating);
    // a small buffer
    start_mod_note(Start_Mod_Given, AAE, note, 4);
    CHECK(strcmp(note, "giv") == 0);
    start_mod_note(Start_Mod_Given, AAE, note, 0);
    CHECK(strcmp(start_mod_plan_text((start_mod_plan_t)99), "?") == 0);
    start_mod_note((start_mod_plan_t)99, AAE, note, sizeof(note));
    CHECK(note[0] == '\0');
}

static void raw_name(char *raw, const char *name) {
    memset(raw, 0, Start_Mod_Name_Size);
    memcpy(raw, name, strlen(name));
}

static void test_menu_ready(void) {
    char raw[Start_Mod_Name_Size];
    start_mod_look_t look;
    fresh_folders();
    // the first menu with the mod loaded: remembered
    raw_name(raw, AAE);
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.loaded == Start_Mod_Loaded_Mod && look.changed && look.written && !look.marker_there);
    CHECK(look.before == Start_Mod_File_Absent);
    CHECK(strcmp(read_text(memory), AAE "\n") == 0);
    // the next menus: nothing to do
    struct stat before_info, after_info;
    stat(memory, &before_info);
    start_mod_menu_ready(&paths, raw, &look);
    stat(memory, &after_info);
    CHECK(!look.changed && look.before == Start_Mod_File_Named && strcmp(look.before_name, AAE) == 0);
    CHECK(before_info.st_ino == after_info.st_ino);
    // the marker of a start given the mod goes at the first menu
    write_text(marker, AAE "\n");
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.marker_there && look.marker_removed && !exists(marker) && !look.changed);
    // another mod loaded from the menus
    raw_name(raw, "zm_mymod");
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.changed && look.written && strcmp(look.before_name, AAE) == 0);
    CHECK(strcmp(read_text(memory), "zm_mymod\n") == 0);
    // unloaded (MODS, Unload): forgotten
    raw_name(raw, "");
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.loaded == Start_Mod_Loaded_None && look.changed && look.written && !exists(memory));
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(!look.changed && look.before == Start_Mod_File_Absent);
    // the stand-in for custom maps, or a name that can't go on a command line: not remembered, and an older one goes
    CHECK(start_mod_write(memory, AAE));
    raw_name(raw, "usermaps");
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.loaded == Start_Mod_Loaded_Stand_In && look.changed && !exists(memory));
    CHECK(start_mod_write(memory, AAE));
    raw_name(raw, "my mod");
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.loaded == Start_Mod_Loaded_Unfit && look.changed && !exists(memory));
    // a damaged file is put right, whatever is loaded
    write_text(memory, "junk junk\n");
    raw_name(raw, "");
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.before == Start_Mod_File_Damaged && look.changed && look.written && !exists(memory));
    write_text(memory, "junk junk\n");
    raw_name(raw, AAE);
    start_mod_menu_ready(&paths, raw, &look);
    CHECK(look.before == Start_Mod_File_Damaged && look.changed && strcmp(read_text(memory), AAE "\n") == 0);
    // a folder that can't be written to: said, and the file as it was
    raw_name(raw, "zm_mymod");
    write_text(marker, AAE "\n");
    chmod(folder, 0555);
    start_mod_menu_ready(&paths, raw, &look);
    chmod(folder, 0755);
    CHECK(look.marker_there && !look.marker_removed && look.changed && !look.written);
    CHECK(strcmp(read_text(memory), AAE "\n") == 0);
    unlink(marker);
    // no folder at all: nothing known of the file, nothing written
    start_mod_paths_t none = { NULL, game, workshop };
    start_mod_menu_ready(&none, raw, &look);
    CHECK(look.changed && !look.written && !look.marker_there && look.before == Start_Mod_File_Damaged);
}

static bool title_of(const char *json, char *out) {
    return start_mod_title_from_json(json, strlen(json), out, Start_Mod_Title_Max + 16);
}

static void test_titles(void) {
    char title[Start_Mod_Title_Max + 16];
    // All-around Enhancement's: the colour codes out, cut at a word to 32 bytes
    CHECK(title_of("{\"Description\": \"x\", \"Title\": \"^1[ZM] ^3All-around Enhancement v4.1.7\", \"Type\": \"mod\"}",
                   title));
    CHECK(strcmp(title, "[ZM] All-around Enhancement") == 0);
    CHECK(title_of("{\"Title\":\"Town Remastered\"}", title) && strcmp(title, "Town Remastered") == 0);
    // escapes, a key that only looks like it inside a string, nested values before it, a byte order mark
    CHECK(title_of("\xef\xbb\xbf{ \"Description\" : \"a \\\"Title\\\": \\\"no\\\"\" , "
                   "\"Tags\": [1, {\"Title\": \"no\"}, "
                   "\"]\"], \"n\": -1.5e3, \"t\": true, \"Title\" : \"Caf\\u00e9 \\\"Mod\\\" \\/ \\\\x\" }",
                   title));
    CHECK(strcmp(title, "Caf\xc3\xa9 \"Mod\" / \\x") == 0);
    // what a notice can't show: control characters and line ends as a space, a lone half of a pair, a 0
    CHECK(title_of("{\"Title\": \"A\\nB\\t\\tC \\ud83d \\u0000 \\u20ac\"}", title));
    CHECK(strcmp(title, "A B C ? ? \xe2\x82\xac") == 0);
    // bytes that aren't UTF-8 as '?', an overlong form too
    CHECK(title_of("{\"Title\": \"x\xff" "y\xc3\"}", title) && strcmp(title, "x?y?") == 0);
    CHECK(title_of("{\"Title\": \"a\xc0\x80" "b\"}", title) && strcmp(title, "a??b") == 0);
    // a long one without a space within reach: cut at a character's start
    CHECK(title_of("{\"Title\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xc3\xa9zzz\"}", title));
    CHECK(strcmp(title, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
    CHECK(title_of("{\"Title\": \"abc defghijklmnopqrstuvwxyzabcdefghijklmnop\"}", title));
    CHECK(strcmp(title, "abc defghijklmnopqrstuvwxyzabcde") == 0 && strlen(title) == Start_Mod_Title_Max);
    CHECK(title_of("{\"Title\": \"   spaced    out   \"}", title) && strcmp(title, "spaced out") == 0);
    // none
    CHECK(!title_of("{\"Title\": \"^1^2^3\"}", title));
    CHECK(!title_of("{\"Title\": 5}", title));
    CHECK(!title_of("{\"Type\": \"mod\"}", title));
    CHECK(!title_of("{}", title));
    CHECK(!title_of("", title));
    CHECK(!title_of("[\"Title\", \"x\"]", title));
    CHECK(!title_of("{\"Title\": \"unended", title));
    CHECK(!title_of("{\"Description\": \"unended, \"Title\": \"x\"", title));
    CHECK(!title_of("{\"Title\": \"bad \\q escape\"}", title));
    CHECK(!title_of("{\"Title\": \"raw\nline\"}", title));
    CHECK(!title_of("{\"Description\" \"no colon\", \"Title\": \"x\"}", title));
    CHECK(!title_of("{\"Tags\": [1, 2, \"Title\": \"x\"}", title));
    CHECK(!title_of("{\"Title\": \"\\u12\"}", title));
    CHECK(!title_of("{\"Title\": \"\\u12g4\"}", title));
    // a small buffer: what fits
    CHECK(start_mod_title_from_json("{\"Title\": \"Town Remastered\"}", 28, title, 5) && strcmp(title, "Town") == 0);
    CHECK(!start_mod_title_from_json("{\"Title\": \"Town\"}", 17, title, 0));
    // the title of a mod as a notice names it
    fresh_folders();
    start_mod_title(&paths, AAE, title, sizeof(title));
    CHECK(strcmp(title, "[ZM] All-around Enhancement") == 0);
    start_mod_title(&paths, "1234", title, sizeof(title));
    CHECK(strcmp(title, "the mod 1234") == 0);
    start_mod_title(&paths, "zm_mymod", title, sizeof(title));
    CHECK(strcmp(title, "the mod zm_mymod") == 0);
    start_mod_title(&paths, "", title, sizeof(title));
    CHECK(strcmp(title, "the mod") == 0);
    start_mod_title(&paths, "../x", title, sizeof(title));
    CHECK(strcmp(title, "the mod") == 0);
}

static void test_notices(void) {
    char text[200], longest[64];
    snprintf(longest, sizeof(longest), "the mod %s", "x234567890123456789012345678901");
    static const start_mod_plan_t with[] = { Start_Mod_Failed, Start_Mod_Missing, Start_Mod_Updating };
    for (size_t i = 0; i < sizeof(with) / sizeof(with[0]); i++) {
        // within a notice's 127 characters with the longest title start_mod_title gives
        CHECK(start_mod_notice_text(with[i], longest, text, sizeof(text)));
        CHECK(strlen(text) <= 127 && strstr(text, longest) != NULL);
        CHECK(start_mod_notice_text(with[i], "[ZM] All-around Enhancement", text, sizeof(text)));
    }
    // how to load it by hand, where it can be
    CHECK(start_mod_notice_text(Start_Mod_Failed, "X", text, sizeof(text)) && strstr(text, "MODS") != NULL);
    CHECK(start_mod_notice_text(Start_Mod_Updating, "X", text, sizeof(text)) && strstr(text, "MODS") != NULL);
    static const start_mod_plan_t without[] = { Start_Mod_Off, Start_Mod_Nothing, Start_Mod_Damaged, Start_Mod_Busy,
                                                Start_Mod_Unsafe, Start_Mod_Given };
    for (size_t i = 0; i < sizeof(without) / sizeof(without[0]); i++) {
        strcpy(text, "untouched");
        CHECK(!start_mod_notice_text(without[i], "X", text, sizeof(text)) && text[0] == '\0');
    }
}

// the game's half, against a made-up block where the game keeps its loaded mod (0x103087858, 0x4d0 bytes): the name
// at +0x84, the mod load's state at +0x4c8
static unsigned char game_block[0x4d0] __attribute__((aligned(16)));
#define game_memory ((char *)game_block + 0x84)
#define game_state (*(int32_t *)(game_block + 0x4c8))
static uint64_t game_base(void) {
    return (uint64_t)(uintptr_t)game_memory - ADDR_Mods_LoadedName;
}

// a first menu, then n polls, what the log got
static const char *menu_and_polls(int polls) {
    capture_start();
    start_mod_game_menu_ready();
    for (int i = 0; i < polls; i++)
        start_mod_game_poll();
    return capture_end();
}

static const char *polls(int count) {
    capture_start();
    for (int i = 0; i < count; i++)
        start_mod_game_poll();
    return capture_end();
}

static void test_game(void) {
    fresh_folders();
    const char *log;
    // the made-up block lies as the game's does
    CHECK(ADDR_Mods_LoadState - ADDR_Mods_LoadedName == 0x4c8 - 0x84);
    // a first menu or a poll before the library was set up (NoCodePatches): nothing
    raw_name(game_memory, AAE);
    game_state = 0;
    log = menu_and_polls(2);
    CHECK(log[0] == '\0' && !exists(memory));
    // the files in a folder of their own (BO3MACFIX_STARTWITHMOD_DIR)
    char own[PATH_MAX];
    path_of(own, root, "own");
    make_folder(own);
    path_of(memory, own, Start_Mod_Memory_File);
    path_of(marker, own, Start_Mod_Marker_File);
    // a start the launcher gave the mod: said in the log, no notice
    posted_count = 0;
    capture_start();
    start_mod_game_init(game_base(), true, own, "given " AAE, false);
    log = capture_end();
    CHECK(strstr(log, "StartWithMod on: " AAE ": started with it") != NULL);
    CHECK(posted_count == 0);
    // its first menus come while the game still loads the mod (state 2): the marker stays, nothing remembered, also
    // through polls and the second first menu of the menus' rebuild
    write_text(marker, AAE "\n");
    game_state = 2;
    log = menu_and_polls(3);
    CHECK(log[0] == '\0' && exists(marker) && !exists(memory));
    log = menu_and_polls(3);
    CHECK(log[0] == '\0' && exists(marker) && !exists(memory));
    // the load done (state 0): the next poll looks, once
    game_state = 0;
    log = polls(1);
    CHECK(strstr(log, "StartWithMod: the start got as far as the menus with no mod load under way\n") != NULL);
    CHECK(strstr(log, "StartWithMod: remembered " AAE " for the next start (was nothing)\n") != NULL);
    CHECK(!exists(marker) && strcmp(read_text(memory), AAE "\n") == 0);
    char default_memory[PATH_MAX];
    path_of(default_memory, folder, Start_Mod_Memory_File);
    CHECK(!exists(default_memory));
    unlink(memory);
    log = polls(3);
    CHECK(log[0] == '\0' && !exists(memory));
    // the next menus, with no load under way: they look at once, quietly when nothing changes
    CHECK(start_mod_write(memory, AAE));
    log = menu_and_polls(0);
    CHECK(log[0] == '\0');
    // a mod that stops the game during its load is never remembered: tried from MODS, it never gets past state 2
    raw_name(game_memory, "zm_crashes");
    game_state = 1;
    log = menu_and_polls(4);
    game_state = 2;
    log = menu_and_polls(4);
    CHECK(log[0] == '\0' && strcmp(read_text(memory), AAE "\n") == 0);
    // a load that starts over (another mod chosen while one loads): the look is for the mod loaded at the end
    raw_name(game_memory, "zm_other");
    game_state = 1;
    log = polls(2);
    CHECK(log[0] == '\0');
    game_state = 0;
    log = polls(2);
    CHECK(strstr(log, "StartWithMod: remembered zm_other for the next start (was " AAE ")\n") != NULL);
    // unloaded (MODS, Unload): ModLoad reset leaves the state at 2 until the game is done, then the mod is forgotten
    raw_name(game_memory, "");
    game_state = 2;
    log = menu_and_polls(2);
    CHECK(log[0] == '\0' && exists(memory));
    game_state = 0;
    log = polls(1);
    CHECK(strstr(log, "StartWithMod: forgot zm_other: no mod is loaded\n") != NULL && !exists(memory));
    // a poll without a first menu before it doesn't look
    CHECK(start_mod_write(memory, AAE));
    log = polls(3);
    CHECK(log[0] == '\0' && exists(memory));
    // a start with no mod at all: state 0 at its first menu, so a stale marker and a stale memory go there and then
    write_text(marker, AAE "\n");
    log = menu_and_polls(0);
    CHECK(strstr(log, "StartWithMod: the start got as far as the menus with no mod load under way\n") != NULL);
    CHECK(strstr(log, "StartWithMod: forgot " AAE ": no mod is loaded\n") != NULL);
    CHECK(!exists(marker) && !exists(memory));
    // the stand-in, with debug lines: at a first menu with no load under way, and at the poll after a load
    CHECK(start_mod_write(memory, AAE));
    posted_count = 0;
    capture_start();
    start_mod_game_init(game_base(), false, own, NULL, true);
    raw_name(game_memory, "usermaps");
    start_mod_game_menu_ready();
    start_mod_game_menu_ready();
    game_state = 2;
    start_mod_game_menu_ready();
    start_mod_game_menu_ready();
    start_mod_game_poll();
    game_state = 0;
    start_mod_game_poll();
    start_mod_game_poll();
    // a look left for later and then made at a first menu with no load under way isn't made again at the next poll
    game_state = 2;
    start_mod_game_menu_ready();
    game_state = 0;
    start_mod_game_menu_ready();
    start_mod_game_poll();
    log = capture_end();
    CHECK(strstr(log, "StartWithMod off: this start didn't come from a launcher that knows it") != NULL);
    CHECK(strstr(log, "forgot " AAE ": only the stock game's stand-in for custom maps is loaded (usermaps)") != NULL);
    CHECK(strstr(log, "StartWithMod: first menu: only the stock game's stand-in for custom maps is loaded (usermaps), "
                      "lastmod.txt holds " AAE ", changed\n") != NULL);
    CHECK(strstr(log, "StartWithMod: first menu: only the stock game's stand-in for custom maps is loaded (usermaps), "
                      "lastmod.txt holds nothing\n") != NULL);
    // said once for the two first menus of one load (and once more for the next load)
    const char *waiting = "StartWithMod: first menu while the game loads a mod (state 2): looking once it's done\n";
    const char *found = strstr(log, waiting);
    CHECK(found != NULL && (found = strstr(found + 1, waiting)) != NULL && strstr(found + 1, waiting) == NULL);
    found = strstr(log, "StartWithMod: the mod's load is done: only the stock game's stand-in for custom maps is "
                        "loaded (usermaps), lastmod.txt holds nothing\n");
    CHECK(found != NULL && strstr(found + 1, "StartWithMod: the mod's load is done") == NULL);
    CHECK(posted_count == 0 && !exists(memory));
    // a start the launcher left the mod out of for a reason the player must know: a lasting notice with its title
    static const struct {
        const char *note, *log, *notice;
    } told[] = {
        { "failed " AAE, "StartWithMod on: " AAE ": the last start with it didn't finish loading it",
          "The last start with [ZM] All-around Enhancement didn't finish loading. This one is without it: load it "
          "from MODS." },
        { "missing zm_mymod", "zm_mymod: its files aren't there",
          "The game started without the mod zm_mymod: it was loaded at the last quit, but its files are gone." },
        { "updating " AAE, "Steam is downloading it",
          "Steam is downloading [ZM] All-around Enhancement, so the game started without it. Load it from MODS once "
          "it's done." },
        { "failed ", "StartWithMod on: the last start with it",
          "The last start with the mod didn't finish loading. This one is without it: load it from MODS." },
    };
    for (size_t i = 0; i < sizeof(told) / sizeof(told[0]); i++) {
        posted_count = 0;
        capture_start();
        start_mod_game_init(game_base(), true, own, told[i].note, false);
        log = capture_end();
        CHECK(strstr(log, told[i].log) != NULL);
        CHECK(posted_count == 1 && strcmp(posted[0], told[i].notice) == 0);
    }
    // the rest only in the log
    static const char *quiet[] = { "off ", "nothing ", "damaged ", "busy " AAE, "unsafe " AAE, "given " AAE };
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
        posted_count = 0;
        capture_start();
        start_mod_game_init(game_base(), true, own, quiet[i], false);
        log = capture_end();
        CHECK(posted_count == 0 && strstr(log, "BO3MacFix: StartWithMod on: ") != NULL);
    }
    // a note this library doesn't know
    posted_count = 0;
    capture_start();
    start_mod_game_init(game_base(), true, own, "given x;quit", false);
    log = capture_end();
    CHECK(posted_count == 0 &&
          strstr(log, "the launcher's note \"given x;quit\" means nothing to this library") != NULL);
    // without a folder of its own: BO3MacFix's, the game's and the workshop's from the game's working folder
    posted_count = 0;
    capture_start();
    start_mod_game_init(game_base(), true, "", "updating " AAE, false);
    log = capture_end();
    CHECK(posted_count == 1 && strstr(posted[0], "[ZM] All-around Enhancement") != NULL);
    raw_name(game_memory, AAE);
    game_state = 0;
    log = menu_and_polls(0);
    CHECK(strstr(log, "remembered " AAE) != NULL && strcmp(read_text(default_memory), AAE "\n") == 0);
    path_of(memory, folder, Start_Mod_Memory_File);
    path_of(marker, folder, Start_Mod_Marker_File);
}

int main(void) {
    const char *temp = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/bo3mf_start_mod_XXXXXX", temp != NULL && temp[0] != '\0' ? temp : "/tmp");
    if (mkdtemp(root) == NULL) {
        printf("FAIL: couldn't make a temporary folder\n");
        return 1;
    }
    test_names();
    test_loaded();
    test_parse();
    test_read_write();
    test_files();
    test_launch();
    test_notes();
    test_menu_ready();
    test_titles();
    test_notices();
    test_game();
    if (chdir("/") == 0)
        remove_tree(root);
    if (failures == 0)
        printf("test_start_mod: all passed\n");
    return failures == 0 ? 0 : 1;
}
