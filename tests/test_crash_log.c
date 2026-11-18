// crash_log.c: the first line written and read back; the quit line found in a log's end; keeping logs in a temporary
// folder (the run before quit, crashed, still runs, or is from an older build; big logs; names near a crash log's
// that must stay; links and folders by a crash log's name); copying; picking macOS's report by time and pid; the time
// lines and the menu lines' limit; and the game's side, with the output in a file as dylib_main makes it: a start
// after a crash from Steam with its report found and summarised into the crash log, the notice, map, menu, time and
// quit lines, the checks' folder taking the runner's file, KeepCrashLogs 0 doing nothing at all, and the menu chunk
// installed into a made-up Lua state.
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/crash_log.c"

// ---- what crash_log.c asks of the rest of BO3MacFix and of the game's Lua
static char notice[256];
static int notices;
void notices_post_lasting(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(notice, sizeof(notice), format, args);
    va_end(args);
    notices++;
}

// a made-up Lua state: the stack's top and base where the game keeps them, a stack of objects
static uint8_t lua_memory[0x80];
static lua_State *const L = (lua_State *)lua_memory;
static HksObject stack[16];
static struct {
    int loads, pcall_arguments;
    const char *chunk_name;
    size_t source_length;
    lua_CFunction pushed;
    bool pushed_boolean;
} lua_asked;
const char *hks_check_string(lua_State *s, int index) {
    HksObject *argument = HKS_BASE(s) + (index - 1);
    return argument < HKS_TOP(s) && argument->t == HKS_TSTRING ? (const char *)argument->v.ptr : NULL;
}
void hks_push_cfunction(lua_State *s, lua_CFunction fn, const char *name) {
    (void)name;
    lua_asked.pushed = fn;
    HKS_TOP(s)++;
}
void hks_push_boolean(lua_State *s, bool value) {
    lua_asked.pushed_boolean = value;
    HKS_TOP(s)++;
}
bool hks_load_source(lua_State *s, const char *source, size_t source_len, const char *chunk_name) {
    (void)source;
    lua_asked.loads++;
    lua_asked.chunk_name = chunk_name;
    lua_asked.source_length = source_len;
    HKS_TOP(s)++;
    return true;
}
// a call that fails, as one whose chunk throws: the function and its arguments go, its error stays on the stack
bool hks_pcall(lua_State *s, int nargs, int nresults) {
    (void)nresults;
    lua_asked.pcall_arguments = nargs;
    HKS_TOP(s) -= nargs + 1;
    HKS_TOP(s)++;
    return false;
}

static void set_stack(int count, const char *first, const char *second) {
    memset(stack, 0, sizeof(stack));
    stack[0].t = first != NULL ? HKS_TSTRING : HKS_TNUMBER;
    stack[0].v.ptr = (void *)first;
    stack[1].t = second != NULL ? HKS_TSTRING : HKS_TNIL;
    stack[1].v.ptr = (void *)second;
    HKS_BASE(L) = stack;
    HKS_TOP(L) = stack + count;
}

// ---- files, in a temporary folder of its own ($TMPDIR, else /tmp)
static char root[1024];

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        printf("FAIL: couldn't write %s\n", path);
        exit(1);
    }
    fputs(text, file);
    fclose(file);
}

static const char *read_file(const char *path) {
    static char text[1 << 20];
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
    struct stat st;
    return lstat(path, &st) == 0;
}

static ino_t inode(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_ino : 0;
}

static void make_folder(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", root, name);
    mkdir(out, 0755);
}

static void in(char *out, size_t size, const char *folder, const char *name) {
    snprintf(out, size, "%s/%s", folder, name);
}

// ---- output to a file and back, as dylib_main sends it to log.txt
static int saved_output = -1;
static void output_to(const char *path) {
    fflush(stdout);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, STDOUT_FILENO);
    close(fd);
}
static void output_back(void) {
    fflush(stdout);
    dup2(saved_output, STDOUT_FILENO);
}

// ---- the first line
static void test_start_line(void) {
    char line[160];
    crash_log_start_t start = { 2026, 11, 13, 18, 25, 1, -8 * 60, 48812 }, back;
    crash_log_format_start(&start, line, sizeof(line));
    CHECK(strcmp(line, "BO3MacFix: log of the start on 2026-11-13 18:25:01 -0800, pid 48812") == 0);
    CHECK(crash_log_parse_start(line, strlen(line), &back) && memcmp(&back, &start, sizeof(start)) == 0);
    CHECK(crash_log_start_epoch(&start) == 1794623101LL);
    crash_log_start_t east = { 2026, 11, 18, 21, 3, 5, 11 * 60, 7 }, half = { 2026, 1, 1, 0, 0, 0, 5 * 60 + 30, 1 };
    CHECK(crash_log_start_epoch(&east) == 1794996185LL && crash_log_start_epoch(&half) == 1767205800LL);
    crash_log_format_start(&half, line, sizeof(line));
    CHECK(strstr(line, " +0530, pid 1") != NULL);
    CHECK(crash_log_parse_start(line, strlen(line), &back) && back.zone_minutes == 330);
    crash_log_start_t west = { 2026, 1, 1, 0, 0, 0, -(3 * 60 + 30), 9 };
    crash_log_format_start(&west, line, sizeof(line));
    CHECK(crash_log_parse_start(line, strlen(line), &back) && back.zone_minutes == -210);

    // found after other lines, with words after it, without a line break; the first one counts
    const char *log = "dyld: something\r\nBO3MacFix: log of the start on 2026-11-13 18:25:01 -0800, pid 48812 "
                      "(KeepCrashLogs)\nBO3MacFix: log of the start on 2027-01-01 00:00:00 +0000, pid 5\n";
    CHECK(crash_log_parse_start(log, strlen(log), &back) && back.pid == 48812 && back.year == 2026);
    CHECK(!crash_log_parse_start(line, strlen(line) - 1, &back)); // "pid " without its number
    const char *bad[] = {
        "BO3MacFix: log of the start on 2026-13-13 18:25:01 -0800, pid 4",     // month 13
        "BO3MacFix: log of the start on 2026-11-13 24:25:01 -0800, pid 4",     // hour 24
        "BO3MacFix: log of the start on 2026-11-13 18:25:01 *0800, pid 4",     // no sign
        "BO3MacFix: log of the start on 2026-11-13 18:25:01 -0860, pid 4",     // 60 minutes
        "BO3MacFix: log of the start on 2026-11-13 18:25:01 -0800, pid 0",     // no pid
        "BO3MacFix: log of the start on 2026-11-13 18:25:01 -0800, pid 4x",    // something stuck to the pid
        "BO3MacFix: log of the start on 2026-11-13 18:25:01 -0800",            // cut off
        "BO3MacFix: log of the start on 2026-1-13 18:25:01 -0800, pid 4",      // not the fixed width
        " BO3MacFix: log of the start on 2026-11-13 18:25:01 -0800, pid 4",    // not at a line's start
        "BO3MacFix: log of the start on 1969-11-13 18:25:01 -0800, pid 4",     // before 1970
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (crash_log_parse_start(bad[i], strlen(bad[i]), &back)) {
            printf("FAIL %s:%d: read a start from \"%s\"\n", __FILE__, __LINE__, bad[i]);
            failures++;
        }
    CHECK(!crash_log_parse_start(NULL, 0, &back));
    // a line longer than any start line is skipped without reading past it
    static char longer[400];
    memset(longer, 'x', sizeof(longer));
    memcpy(longer, Crash_Log_Start_Line, strlen(Crash_Log_Start_Line));
    CHECK(!crash_log_parse_start(longer, sizeof(longer), &back));

    char name[48];
    crash_log_crash_name(&start, name, sizeof(name));
    CHECK(strcmp(name, "crash-20261113-182501.txt") == 0);
    CHECK(crash_log_is_crash_name(name));
    const char *not_names[] = { "crash-20261113-18250.txt", "crash-20261113-1825011.txt", "crash-2026111x-182501.txt",
                                "crash-20261113_182501.txt", "Crash-20261113-182501.txt", "crash-20261113-182501.TXT",
                                "crash-20261113-182501.txt.bak", "xcrash-20261113-182501.txt", "crash-.txt", "",
                                "crash-20261113-182501.tx", "log.txt" };
    for (size_t i = 0; i < sizeof(not_names) / sizeof(not_names[0]); i++)
        if (crash_log_is_crash_name(not_names[i])) {
            printf("FAIL %s:%d: \"%s\" taken for a crash log\n", __FILE__, __LINE__, not_names[i]);
            failures++;
        }
}

// ---- the quit line in a log's end
static void test_quit_line(void) {
    const char *quit = "a\n" Crash_Log_Quit_Line ", exit status 0 (time 10:00:00)\nafter it\n";
    CHECK(crash_log_quit_in_tail(quit, strlen(quit), false));
    CHECK(crash_log_quit_in_tail(Crash_Log_Quit_Line "\n", strlen(Crash_Log_Quit_Line) + 1, true));
    CHECK(!crash_log_quit_in_tail(Crash_Log_Quit_Line "\n", strlen(Crash_Log_Quit_Line) + 1, false));
    const char *inside = "a line about " Crash_Log_Quit_Line "\n";
    CHECK(!crash_log_quit_in_tail(inside, strlen(inside), true));
    const char *cut = "a\n" Crash_Log_Quit_Line;
    CHECK(crash_log_quit_in_tail(cut, strlen(cut), false));
    CHECK(!crash_log_quit_in_tail(cut, strlen(cut) - 1, false));
    CHECK(!crash_log_quit_in_tail("", 0, true) && !crash_log_quit_in_tail(NULL, 5, true));
    const char *older = "BO3MacFix: the game quit with status 0, ending it without running static destructors\n";
    CHECK(!crash_log_quit_in_tail(older, strlen(older), true));
}

// ---- keeping logs
static int running_asked;
static bool running_answer;
static bool fake_running(int pid) {
    (void)pid;
    running_asked++;
    return running_answer;
}

static const char crashed_log[] = "BO3MacFix: log of the start on 2026-11-13 18:25:01 -0800, pid 48812 (KeepCrashLogs)\n"
                                  "menu: ZMGlobeMapSelect opened over ChangeMap\nmalloc: *** error for object\n";
static const char quit_log[] = "BO3MacFix: log of the start on 2026-11-14 10:24:00 -0800, pid 5000 (KeepCrashLogs)\n"
                               "lines\n" Crash_Log_Quit_Line ", exit status 0 (time 11:00:00)\n"
                               "BO3MacFix: pre-patched executable at quit: 0 refused\n";

static void test_keep(void) {
    char folder[1024], path[1200], previous[1200], crash[1200];
    crash_log_kept_t kept;

    // no log: nothing to judge, old crash logs still pruned
    make_folder(folder, sizeof(folder), "none");
    for (int i = 1; i <= 3; i++) {
        snprintf(path, sizeof(path), "%s/crash-2025010%d-000000.txt", folder, i);
        write_file(path, "old");
    }
    crash_log_keep(folder, 2, fake_running, &kept);
    CHECK(kept.previous == Previous_None && !kept.moved && kept.removed == 1);
    in(path, sizeof(path), folder, "crash-20250101-000000.txt");
    CHECK(!exists(path));
    in(path, sizeof(path), folder, "crash-20250103-000000.txt");
    CHECK(exists(path));

    // from a build without the first line: moved, not judged
    make_folder(folder, sizeof(folder), "unknown");
    in(path, sizeof(path), folder, "log.txt");
    in(previous, sizeof(previous), folder, "log-previous.txt");
    write_file(path, "BO3MacFix: offline mode: ...\nBO3MacFix: the game quit with status 0\n");
    write_file(previous, "the one before that");
    running_asked = 0;
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Unknown && kept.moved && kept.crash_name[0] == '\0' && running_asked == 0);
    CHECK(!exists(path) && strstr(read_file(previous), "offline mode") != NULL);

    // ended by quitting
    make_folder(folder, sizeof(folder), "quit");
    in(path, sizeof(path), folder, "log.txt");
    in(previous, sizeof(previous), folder, "log-previous.txt");
    write_file(path, quit_log);
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Quit && kept.moved && kept.crash_name[0] == '\0' && running_asked == 0);
    CHECK(kept.start.pid == 5000 && kept.start.day == 14);
    CHECK(strcmp(read_file(previous), quit_log) == 0 && !exists(path));
    DIR *dir = opendir(folder);
    struct dirent *entry;
    int files = 0;
    while ((entry = readdir(dir)) != NULL)
        files += entry->d_name[0] != '.';
    closedir(dir);
    CHECK(files == 1);

    // still running (two copies of the game)
    make_folder(folder, sizeof(folder), "running");
    in(path, sizeof(path), folder, "log.txt");
    write_file(path, crashed_log);
    running_answer = true;
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Running && kept.moved && kept.crash_name[0] == '\0' && running_asked == 1);
    running_answer = false;

    // didn't end by quitting: log-previous.txt and a second name for it, the crash log
    make_folder(folder, sizeof(folder), "crashed");
    in(path, sizeof(path), folder, "log.txt");
    in(previous, sizeof(previous), folder, "log-previous.txt");
    in(crash, sizeof(crash), folder, "crash-20261113-182501.txt");
    write_file(path, crashed_log);
    write_file(previous, "older");
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Crashed && kept.moved && strcmp(kept.crash_name, "crash-20261113-182501.txt") == 0);
    CHECK(kept.start.pid == 48812 && !exists(path));
    CHECK(strcmp(read_file(crash), crashed_log) == 0 && inode(crash) == inode(previous));
    // the next start after that one: log-previous.txt is replaced, the crash log stays
    write_file(path, quit_log);
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Quit && strcmp(read_file(crash), crashed_log) == 0);
    CHECK(strcmp(read_file(previous), quit_log) == 0);
    // a crash log by that name is there already (that run's, kept before): it stays as it is
    write_file(path, crashed_log);
    write_file(crash, "kept before");
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Crashed && strcmp(kept.crash_name, "crash-20261113-182501.txt") == 0);
    CHECK(strcmp(read_file(crash), "kept before") == 0 && strcmp(read_file(previous), crashed_log) == 0);

    // a folder nothing can be written in: judged, but neither moved nor kept apart, and said so (no crash log named)
    make_folder(folder, sizeof(folder), "readonly");
    in(path, sizeof(path), folder, "log.txt");
    write_file(path, crashed_log);
    chmod(folder, 0555);
    crash_log_keep(folder, 10, fake_running, &kept);
    chmod(folder, 0755);
    if (getuid() != 0)
        CHECK(kept.previous == Previous_Crashed && !kept.moved && kept.crash_name[0] == '\0' && exists(path));

    // a log.txt that is a folder: left alone
    make_folder(folder, sizeof(folder), "folder");
    in(path, sizeof(path), folder, "log.txt");
    mkdir(path, 0755);
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_None && !kept.moved && exists(path));

    // a big log: the first line within its first 16 KB, the quit line within its last 256 KB
    static char big[1 << 20];
    make_folder(folder, sizeof(folder), "big");
    in(path, sizeof(path), folder, "log.txt");
    memset(big, 'x', sizeof(big) - 1);
    for (size_t i = 99; i < sizeof(big) - 1; i += 100)
        big[i] = '\n';
    const char *first = "BO3MacFix: log of the start on 2026-11-13 18:25:01 -0800, pid 48812\n";
    memcpy(big + 10000, first, strlen(first));
    memcpy(big + 300, Crash_Log_Quit_Line, strlen(Crash_Log_Quit_Line)); // far from the end: not seen
    write_file(path, big);
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Crashed);
    memcpy(big + 848600, Crash_Log_Quit_Line, strlen(Crash_Log_Quit_Line)); // a line's start 200 KB from the end
    write_file(path, big);
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Quit);
    memcpy(big + 10000, "nothing to see, nothing to see, nothing to see, nothing to see, nothing", strlen(first) - 1);
    memcpy(big + 20000, first, strlen(first));
    write_file(path, big);
    crash_log_keep(folder, 10, fake_running, &kept);
    CHECK(kept.previous == Previous_Unknown);
}

// ---- removing old crash logs: only crash logs, the oldest first
static void test_prune(void) {
    char folder[1024], path[1200], outside[1200];
    make_folder(folder, sizeof(folder), "prune");
    for (int i = 0; i < 12; i++) {
        snprintf(path, sizeof(path), "%s/crash-2025%02d01-120000.txt", folder, i + 1);
        write_file(path, "crash");
    }
    // names in other cases are on dates of their own: on a disk that ignores case (APFS as macOS sets it up) they would
    // be the crash logs of their date
    const char *near[] = { "crash-20250101-12000.txt", "crash-20250101-120000.txt.bak", "Crash-20240101-120000.txt",
                           "crash-20240102-120000.TXT", "xcrash-20250101-120000.txt", "crash-2025010a-120000.txt",
                           "crash-20250101_120000.txt", "log-previous.txt.keep", "logs.txt", "notes.txt",
                           "settings.ini", "checkpoint.ini", "log.txt", "log-previous.txt", "crash-notes.txt" };
    for (size_t i = 0; i < sizeof(near) / sizeof(near[0]); i++) {
        in(path, sizeof(path), folder, near[i]);
        write_file(path, "keep me");
    }
    // a folder by a crash log's name stays, a link by one goes but not what it points to
    in(path, sizeof(path), folder, "crash-20000101-000000.txt");
    mkdir(path, 0755);
    in(outside, sizeof(outside), root, "outside.txt");
    write_file(outside, "outside");
    in(path, sizeof(path), folder, "crash-19990101-000000.txt");
    CHECK(symlink(outside, path) == 0);
    CHECK(crash_log_prune(folder, 0) == 0 && crash_log_prune(folder, -3) == 0);
    int removed = crash_log_prune(folder, 10);
    CHECK(removed == 3); // two files and the link; the folder can't go
    for (int i = 0; i < 12; i++) {
        snprintf(path, sizeof(path), "%s/crash-2025%02d01-120000.txt", folder, i + 1);
        CHECK(exists(path) == (i >= 2));
    }
    for (size_t i = 0; i < sizeof(near) / sizeof(near[0]); i++) {
        in(path, sizeof(path), folder, near[i]);
        if (!exists(path) || strcmp(read_file(path), "keep me") != 0) {
            printf("FAIL %s:%d: %s was touched\n", __FILE__, __LINE__, near[i]);
            failures++;
        }
    }
    in(path, sizeof(path), folder, "crash-20000101-000000.txt");
    CHECK(exists(path));
    in(path, sizeof(path), folder, "crash-19990101-000000.txt");
    CHECK(!exists(path) && strcmp(read_file(outside), "outside") == 0);
    CHECK(crash_log_prune(folder, 1) == 9 && crash_log_prune(folder, 1) == 0);
    snprintf(path, sizeof(path), "%s/crash-20251201-120000.txt", folder);
    CHECK(exists(path));
    CHECK(crash_log_prune("/nonexistent/folder", 1) == 0);
}

// ---- copying, where a second name can't be made
static void test_copy(void) {
    char folder[1024], from[1200], to[1200], link_path[1200];
    make_folder(folder, sizeof(folder), "copy");
    in(from, sizeof(from), folder, "from.txt");
    in(to, sizeof(to), folder, "to.txt");
    static char text[300000];
    for (size_t i = 0; i < sizeof(text) - 1; i++)
        text[i] = (char)('a' + i % 26);
    write_file(from, text);
    CHECK(crash_log_copy(from, to, Copy_Limit) && strcmp(read_file(to), text) == 0);
    CHECK(!crash_log_copy(from, to, Copy_Limit) && strcmp(read_file(to), text) == 0); // never over a file
    unlink(to);
    CHECK(crash_log_copy(from, to, 1000) && strlen(read_file(to)) == 1000);
    unlink(to);
    in(link_path, sizeof(link_path), folder, "link.txt");
    CHECK(symlink(from, link_path) == 0);
    CHECK(!crash_log_copy(link_path, to, Copy_Limit) && !exists(to)); // a link isn't followed
    CHECK(!crash_log_copy("/nonexistent", to, Copy_Limit) && !exists(to));
}

// ---- macOS's report, picked by time and pid
// the game where Steam puts it, for someone, as the running game finds its two programs
#define Game_Folder "/Users/someone/Library/Application Support/Steam/steamapps/common/Call of Duty Black Ops III/" \
                    "CoDBlkOps3.app/Contents/MacOS"
static const char *const the_game[] = { Game_Folder "/CoDBlkOps3_Exe", Game_Folder "/CoDBlkOps3_Exe.bo3macfix", NULL };

static const char report_of_48812[] =
    "{\"bug_type\":\"309\"}\n{\"pid\":48812,\"procName\":\"CoDBlkOps3_Exe.bo3macfix\",\"procPath\":\"\\/Users\\/USER"
    "\\/Library\\/Application Support\\/Steam\\/*\\/CoDBlkOps3.app\\/Contents\\/MacOS\\/CoDBlkOps3_Exe.bo3macfix\","
    "\"exception\":{\"type\":"
    "\"EXC_BAD_ACCESS\",\"signal\":\"SIGSEGV\",\"subtype\":\"KERN_INVALID_ADDRESS at 0x0000000000000278\"},"
    "\"faultingThread\":0,\"threads\":[{\"name\":\"Main\",\"threadState\":{\"rip\":{\"value\":4312458481}},"
    "\"frames\":[{\"imageOffset\":17475803,\"imageIndex\":0}]}],\"usedImages\":[{\"name\":\"CoDBlkOps3_Exe\"}]}\n";

static void set_time(const char *path, long long when) {
    struct timeval times[2] = { { (time_t)when, 0 }, { (time_t)when, 0 } };
    utimes(path, times);
}

static void test_find_report(void) {
    char system_folder[1024], user_folder[1024], path[1200], found[1200], summary[4096];
    make_folder(system_folder, sizeof(system_folder), "reports-system");
    make_folder(user_folder, sizeof(user_folder), "reports-user");
    const char *folders[] = { system_folder, user_folder, "/nonexistent" };
    long long since = 1794623101LL;
    CHECK(crash_log_find_report(folders, 3, since, 48812, the_game, found, sizeof(found), summary, sizeof(summary)) == Report_None);
    // the right pid but written before the run began; another pid; another name; a link
    in(path, sizeof(path), system_folder, "CoDBlkOps3_Exe.bo3macfix-2026-11-01-000000.ips");
    write_file(path, report_of_48812);
    set_time(path, since - 86400);
    char other[sizeof(report_of_48812)];
    snprintf(other, sizeof(other), "%s", report_of_48812);
    memcpy(strstr(other, "48812"), "48813", 5);
    in(path, sizeof(path), system_folder, "CoDBlkOps3_Exe-2026-11-13-182700.ips");
    write_file(path, other);
    in(path, sizeof(path), user_folder, "Other-2026-11-13-182631.ips");
    write_file(path, report_of_48812);
    in(path, sizeof(path), user_folder, "CoDBlkOps3_Exe.bo3macfix-2026-11-13-182631.ips.tmp");
    write_file(path, report_of_48812);
    in(path, sizeof(path), user_folder, "CoDBlkOps3_Exe-link.ips");
    char linked[1200];
    in(linked, sizeof(linked), root, "linked-report.ips");
    write_file(linked, report_of_48812);
    CHECK(symlink(linked, path) == 0);
    CHECK(crash_log_find_report(folders, 3, since, 48812, the_game, found, sizeof(found), summary, sizeof(summary)) == Report_None);
    CHECK(summary[0] == '\0');
    // one of that time cut off: named when nothing better is there
    in(path, sizeof(path), user_folder, "CoDBlkOps3_Exe.bo3macfix-2026-11-13-182630.ips");
    write_file(path, "{\"bug_type\":\"309\"}\n{\"pid\":48812,\"exception\":{\"ty");
    CHECK(crash_log_find_report(folders, 3, since, 48812, the_game, found, sizeof(found), summary, sizeof(summary)) ==
          Report_Unreadable);
    CHECK(strcmp(found, path) == 0 && strstr(summary, "cut off") != NULL);
    // the one: the right pid, written after the run began (a second before it counts, clocks being in seconds)
    in(path, sizeof(path), user_folder, "CoDBlkOps3_Exe.bo3macfix-2026-11-13-182631.ips");
    write_file(path, report_of_48812);
    set_time(path, since - 1);
    CHECK(crash_log_find_report(folders, 3, since, 48812, the_game, found, sizeof(found), summary, sizeof(summary)) ==
          Report_Summarised);
    CHECK(strcmp(found, path) == 0);
    CHECK(strstr(summary, "crashed thread 0 \"Main\", rip 0x1010ae4f1\n   0  game 0x1010aa8db\n") != NULL);
    // and not for another pid
    CHECK(crash_log_find_report(folders, 3, since, 48814, the_game, found, sizeof(found), summary, sizeof(summary)) ==
          Report_Unreadable);
    // nor for a program by the game's name elsewhere (a stand-in a test ran): only the game's own programs count
    const char *const stand_in[] = { "/private/tmp/stand-in/CoDBlkOps3_Exe.bo3macfix", NULL };
    CHECK(crash_log_find_report(folders, 3, since, 48812, stand_in, found, sizeof(found), summary, sizeof(summary)) ==
          Report_Unreadable);
    set_time(path, since - 10);
    char cut[1200];
    in(cut, sizeof(cut), user_folder, "CoDBlkOps3_Exe.bo3macfix-2026-11-13-182630.ips");
    unlink(cut);
    CHECK(crash_log_find_report(folders, 3, since, 48812, the_game, found, sizeof(found), summary, sizeof(summary)) == Report_None);
}

// ---- the time lines and the menu lines' limit
static void test_lines(void) {
    char text[200];
    crash_log_state_text(NULL, text, sizeof(text));
    CHECK(strcmp(text, "starting") == 0);
    crash_log_state_text("", text, sizeof(text));
    CHECK(strcmp(text, "starting") == 0);
    crash_log_state_text("core_frontend", text, sizeof(text));
    CHECK(strcmp(text, "in the menus") == 0);
    crash_log_state_text("zm_factory", text, sizeof(text));
    CHECK(strcmp(text, "in a game on zm_factory") == 0);
    struct tm local = { .tm_hour = 9, .tm_min = 5, .tm_sec = 3 };
    crash_log_format_time(&local, 65.26, 7234, "core_frontend", text, sizeof(text));
    CHECK(strcmp(text, "time 09:05:03, 65.3 s since the start, frame 7234, in the menus") == 0);
    crash_log_format_time(&local, 0.04, 0, NULL, text, sizeof(text));
    CHECK(strcmp(text, "time 09:05:03, 0.0 s since the start, starting") == 0);

    crash_log_limit_t limit = { 0 };
    int shown = 0;
    for (int i = 0; i < 50; i++)
        shown += crash_log_limit(&limit, 100.0 + i * 0.1);
    CHECK(shown == Crash_Log_Menu_Lines && limit.left_out == 10);
    CHECK(!crash_log_limit(&limit, 159.9) && limit.left_out == 11);
    CHECK(crash_log_limit(&limit, 160.0) && limit.lines == 1); // a new minute
    CHECK(crash_log_limit(&limit, 50.0) && limit.lines == 1);  // a clock that went back starts over
}

// ---- the game's side
static uint64_t frame_counter;
static uint64_t made_up_base(void) {
    return (uint64_t)(uintptr_t)&frame_counter - ADDR_rt_frame_counter;
}

// the line of text that starts with start, "" if there's none
static const char *line_starting(const char *text, const char *start) {
    static char line[1024];
    size_t length = strlen(start);
    for (const char *at = text; at != NULL && *at != '\0';) {
        if (strncmp(at, start, length) == 0) {
            snprintf(line, sizeof(line), "%.*s", (int)strcspn(at, "\n"), at);
            return line;
        }
        at = strchr(at, '\n');
        if (at != NULL)
            at++;
    }
    return "";
}

static bool ends_with(const char *text, const char *end) {
    size_t length = strlen(text), end_length = strlen(end);
    return length >= end_length && strcmp(text + length - end_length, end) == 0;
}

// waits up to seconds for text to show in the file at path
static bool shows(const char *path, const char *text, double seconds) {
    for (double waited = 0; waited < seconds; waited += 0.02) {
        const char *now_text = read_file(path);
        if (now_text != NULL && strstr(now_text, text) != NULL)
            return true;
        usleep(20000);
    }
    return false;
}

// KeepCrashLogs 0: everything as before, nothing written, nothing touched
static void test_off(void) {
    char folder[1024], path[1200], out[1200];
    make_folder(folder, sizeof(folder), "off");
    in(path, sizeof(path), folder, "log.txt");
    write_file(path, crashed_log);
    in(out, sizeof(out), root, "off-output.txt");
    output_to(out);
    const char *log_path = crash_log_prepare(0, folder, true);
    crash_log_started(made_up_base(), "/nonexistent/settings.ini");
    crash_log_notice();
    crash_log_map("zm_factory");
    crash_log_quit(0);
    lua_asked.loads = 0;
    set_stack(0, NULL, NULL);
    crash_log_lua_install(L);
    output_back();
    CHECK(strcmp(log_path, path) == 0);
    CHECK(strcmp(read_file(out), "") == 0);
    CHECK(strcmp(read_file(path), crashed_log) == 0);
    in(path, sizeof(path), folder, "log-previous.txt");
    CHECK(!exists(path));
    CHECK(notices == 0 && lua_asked.loads == 0 && !state.enabled);
}

// a start from Steam after a crash: everything as the game would do it, in made-up folders
static void test_after_crash(void) {
    char folder[1024], path[1200], crash[1200], settings[1200], system_folder[1024], user_folder[1024], report[1200];
    make_folder(folder, sizeof(folder), "BO3MacFix");
    make_folder(system_folder, sizeof(system_folder), "steam-reports-system");
    make_folder(user_folder, sizeof(user_folder), "steam-reports-user");
    in(path, sizeof(path), folder, "log.txt");
    write_file(path, crashed_log);
    for (int i = 0; i < 10; i++) {
        snprintf(crash, sizeof(crash), "%s/crash-2025%02d01-000000.txt", folder, i + 1);
        write_file(crash, "an older crash");
    }
    in(settings, sizeof(settings), folder, "settings.ini");
    write_file(settings, "// BO3MacFix's settings\n// ExitFix = 0 would turn it off\nExitFix = 1\n  BatteryFPS = 40\n\n"
                         "OfflineMode=1\n# Notices = 0\n");
    in(report, sizeof(report), user_folder, "CoDBlkOps3_Exe.bo3macfix-2026-11-13-182631.ips");
    report_folders[0] = system_folder;
    report_folders[1] = user_folder;
    game_executables[0] = the_game[0];
    game_executables[1] = the_game[1];
    clock_interval = 0.25;
    frame_counter = 777;

    const char *log_path = crash_log_prepare(10, folder, true);
    CHECK(strcmp(log_path, path) == 0 && state.enabled && state.rotating && !state.test);
    CHECK(state.kept.previous == Previous_Crashed && strcmp(state.kept.crash_name, "crash-20261113-182501.txt") == 0);
    CHECK(state.kept.removed == 1 && !exists(path));
    output_to(log_path); // what dylib_main's freopen does
    crash_log_started(made_up_base(), settings);
    in(crash, sizeof(crash), folder, "crash-20261113-182501.txt");
    // macOS writes the report a moment after the game starts again: the first look finds nothing and says nothing,
    // the second, a clock_interval later, finds it
    usleep(100000);
    CHECK(strstr(read_file(crash), "---- added by BO3MacFix") == NULL);
    write_file(report, report_of_48812);
    CHECK(shows(crash, "rip 0x1010ae4f1", 3.0));
    crash_log_notice();
    crash_log_map("core_frontend");
    set_stack(2, "menu: Lobby opened", "Lobby");
    lua_menu_line(L);
    set_stack(2, "menu: ChangeMap opened over Lobby", "ChangeMap");
    lua_menu_line(L);
    set_stack(1, NULL, NULL); // not a string: nothing
    lua_menu_line(L);
    CHECK(shows(log_path, ", menu ChangeMap\n", 3.0));
    for (int i = 0; i < Crash_Log_Menu_Lines; i++) {
        set_stack(2, "menu: Spam opened over ChangeMap", "Spam");
        lua_menu_line(L);
    }
    CHECK(shows(log_path, ", menu Spam, 2 menu lines left out\n", 3.0));
    crash_log_quit(0);
    crash_log_quit(1);
    clock_interval = 1e9; // no more time lines
    usleep(400000);
    output_back();

    const char *log = read_file(log_path);
    // CRASHLOG_TEST_SHOW=1 prints the log and the crash log, to look at
    if (getenv("CRASHLOG_TEST_SHOW") != NULL) {
        printf("---- log.txt\n%s", log);
        printf("---- %s\n%s----\n", crash, read_file(crash));
    }
    log = read_file(log_path);
    crash_log_start_t start;
    CHECK(crash_log_parse_start(log, strlen(log), &start) && start.pid == getpid());
    CHECK(strncmp(log, Crash_Log_Start_Line, strlen(Crash_Log_Start_Line)) == 0); // the very first line
    CHECK(strstr(log, " (KeepCrashLogs)\nBO3MacFix: build " MACFIX_VERSION ", ") != NULL);
    CHECK(strstr(log, ", macOS ") != NULL && strstr(log, "settings.ini read (3 settings in it)\n") != NULL);
    CHECK(strstr(log, "BO3MacFix: crash logs: the run before (started 2026-11-13 18:25:01, pid 48812) didn't end by "
                      "quitting (a crash, a force quit or a power cut); its log is kept as crash-20261113-182501.txt") !=
          NULL);
    CHECK(strstr(log, "BO3MacFix: crash logs: removed the 1 oldest, keeping the newest 10 (KeepCrashLogs)\n") != NULL);
    CHECK(strstr(log, "BO3MacFix: crash logs: macOS's report of the run before is ") != NULL);
    CHECK(strstr(log, "/steam-reports-user/CoDBlkOps3_Exe.bo3macfix-2026-11-13-182631.ips; exception: EXC_BAD_ACCESS, "
                      "SIGSEGV, KERN_INVALID_ADDRESS at 0x0000000000000278; crashed thread 0 \"Main\", rip 0x1010ae4f1; "
                      "its summary is in crash-20261113-182501.txt\n") != NULL);
    CHECK(ends_with(line_starting(log, "BO3MacFix: map core_frontend (time "), " s since the start, frame 777, in the menus)"));
    CHECK(ends_with(line_starting(log, "BO3MacFix: menu: Lobby opened (time "), ", frame 777, in the menus)"));
    CHECK(ends_with(line_starting(log, "BO3MacFix: menu: ChangeMap opened over Lobby (time "), ", in the menus)"));
    CHECK(ends_with(line_starting(log, "BO3MacFix: time "), ", frame 777, in the menus, menu ChangeMap") ||
          ends_with(line_starting(log, "BO3MacFix: time "), ", starting"));
    // 2 lines and 38 of the 40 Spam lines in the minute
    int spam = 0;
    for (const char *at = log; (at = strstr(at, "menu: Spam opened")) != NULL; at++)
        spam++;
    CHECK(spam == Crash_Log_Menu_Lines - 2);
    CHECK(strstr(log, "\n" Crash_Log_Quit_Line ", exit status 0 (time ") != NULL);
    // the left out lines are counted in one time line, then start over
    int left_out_lines = 0;
    for (const char *at = log; (at = strstr(at, "menu lines left out")) != NULL; at++)
        left_out_lines++;
    CHECK(left_out_lines == 1);
    CHECK(strstr(log, "exit status 1") == NULL);
    // the next start will see that this one ended by quitting
    CHECK(crash_log_quit_in_tail(log, strlen(log), true));

    const char *kept = read_file(crash);
    CHECK(strncmp(kept, crashed_log, strlen(crashed_log)) == 0);
    CHECK(strstr(kept, "\n---- added by BO3MacFix at the next start, ") != NULL);
    CHECK(strstr(kept, ": macOS's crash report of this run, ") != NULL);
    CHECK(strstr(kept, "process: CoDBlkOps3_Exe.bo3macfix, pid 48812, started ?, crashed ?\nexception: EXC_BAD_ACCESS, "
                       "SIGSEGV, KERN_INVALID_ADDRESS at 0x0000000000000278\ncrashed thread 0 \"Main\", rip 0x1010ae4f1\n"
                       "   0  game 0x1010aa8db\n") != NULL);
    CHECK(notices == 1 && strcmp(notice, "The game ended without quitting last time. Its log: "
                                         "BO3MacFix/crash-20261113-182501.txt") == 0);

    // the Lua: loaded with the line function and the debug switch, the stack as it was
    set_stack(0, NULL, NULL);
    lua_asked.loads = 0;
    crash_log_lua_install(L);
    CHECK(lua_asked.loads == 1 && strcmp(lua_asked.chunk_name, "=BO3MacFix crash log") == 0);
    CHECK(lua_asked.source_length == crash_log_lua_len && lua_asked.pushed == lua_menu_line);
    CHECK(lua_asked.pcall_arguments == 2 && !lua_asked.pushed_boolean && HKS_TOP(L) == stack);
}

// macOS wrote no report: the last look says so in the crash log and the log, the first says nothing
static void test_no_report(void) {
    char folder[1024], crash[1200], out[1200], empty_folder[1024];
    make_folder(folder, sizeof(folder), "noreport");
    make_folder(empty_folder, sizeof(empty_folder), "noreport-reports");
    in(crash, sizeof(crash), folder, "crash-20261118-210305.txt");
    write_file(crash, "the log of a run that was killed\n");
    snprintf(state.folder, sizeof(state.folder), "%s", folder);
    snprintf(state.kept.crash_name, sizeof(state.kept.crash_name), "crash-20261118-210305.txt");
    state.kept.start = (crash_log_start_t){ 2026, 11, 18, 21, 3, 5, 660, 4242 };
    report_folders[0] = empty_folder;
    report_folders[1] = NULL;
    in(out, sizeof(out), root, "noreport-output.txt");
    output_to(out);
    bool done = look_for_report(false);
    output_back();
    CHECK(!done && strcmp(read_file(out), "") == 0);
    CHECK(strcmp(read_file(crash), "the log of a run that was killed\n") == 0);
    output_to(out);
    done = look_for_report(true);
    output_back();
    CHECK(done);
    CHECK(strstr(read_file(out), "BO3MacFix: crash logs: no crash report of macOS for the run before (pid 4242) a minute "
                                 "after this start; a force quit or a power cut leaves none\n") != NULL);
    CHECK(strstr(read_file(crash), "macOS wrote no crash report of this run (pid 4242) in ") != NULL);
}

// the checks' folder: the runner's own file becomes its log.txt
static void test_checks_folder(void) {
    char folder[1024], output[1200], log_path[1200];
    make_folder(folder, sizeof(folder), "checks");
    snprintf(state.folder, sizeof(state.folder), "%s", folder);
    in(log_path, sizeof(log_path), folder, "log.txt");
    write_file(log_path, "an old log.txt the start couldn't move");
    in(output, sizeof(output), root, "runner.log");
    output_to(output);
    link_output();
    printf("a line of this run\n");
    output_back();
    struct stat link_stat;
    CHECK(lstat(log_path, &link_stat) == 0 && S_ISREG(link_stat.st_mode)); // a second name, not a symbolic link
    CHECK(inode(log_path) == inode(output) && inode(output) != 0);
    CHECK(strstr(read_file(log_path), "(BO3MACFIX_CRASHLOGDIR), this run's log is its log.txt\na line of this run\n") !=
          NULL);
    // where the output can't be named (a pipe), the line says so
    int pipe_ends[2];
    CHECK(pipe(pipe_ends) == 0);
    fflush(stdout);
    dup2(pipe_ends[1], STDOUT_FILENO);
    link_output();
    output_back();
    char said[512] = "";
    ssize_t got = read(pipe_ends[0], said, sizeof(said) - 1);
    said[got > 0 ? got : 0] = '\0';
    CHECK(strstr(said, "isn't this run's") != NULL || strstr(said, "couldn't give") != NULL);
    close(pipe_ends[0]);
    close(pipe_ends[1]);
}

// whether a pid is a running copy of the game: by its name, never this process. No program by the game's name is
// started for it (macOS would keep a report of one that died, by the game's name): the program that started the
// test (make, a shell) stands for the game, under the name the test gives the game.
static void test_running(void) {
    char parent[64] = "";
    pid_t parent_pid = getppid();
    CHECK(!game_running(parent_pid)); // make or a shell isn't the game
    if (parent_pid > 1 && proc_name(parent_pid, parent, sizeof(parent)) > 0) {
        game_name = parent;
        CHECK(game_running(parent_pid));
        game_name = "CoDBlkOps3_Exe";
    }
    // this one never counts, whatever it is called (after a restart the pid of the run before can be this one's)
    char self[64] = "";
    if (proc_name(getpid(), self, sizeof(self)) > 0) {
        game_name = self;
        CHECK(!game_running(getpid()));
        game_name = "CoDBlkOps3_Exe";
    }
    CHECK(!game_running(getpid()) && !game_running(0) && !game_running(-5) && !game_running(999999999));
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    saved_output = dup(STDOUT_FILENO);
    const char *temporary = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/bo3mf_crashlog_XXXXXX",
             temporary != NULL && temporary[0] != '\0' ? temporary : "/tmp");
    if (mkdtemp(root) == NULL) {
        printf("FAIL: no temporary folder\n");
        return 1;
    }
    test_start_line();
    test_quit_line();
    test_keep();
    test_prune();
    test_copy();
    test_find_report();
    test_lines();
    test_running();
    test_off();
    test_after_crash();
    test_no_report();
    test_checks_folder();
    char command[1200];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    system(command);
    if (failures == 0)
        printf("crash_log: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
