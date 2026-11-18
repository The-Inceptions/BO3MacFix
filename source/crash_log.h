// KeepCrashLogs (crash_log.c): the log of a run of the game that didn't end by quitting (a crash, a force quit, a power
// cut) is kept as BO3MacFix/crash-<the date and time it started>.txt, with a summary of macOS's crash report added at
// the next start (crash_report.c), and the player is told by a notice; the log of the run before is always kept as
// log-previous.txt. The log says when, of which build, on which macOS and power the game started, and on its way the
// time once a minute and whenever a menu opens or closes or a map loads. With KeepCrashLogs = 0 none of this happens.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "havok.h"

// the line every orderly way out of the game writes (exit(), qol.c), and the first line of each log
#define Crash_Log_Quit_Line "BO3MacFix: ended by quitting"
#define Crash_Log_Start_Line "BO3MacFix: log of the start on "
// how much of the end of a log the quit line is looked for in, and of its start the first line
#define Crash_Log_Tail (256 * 1024)
#define Crash_Log_Head (16 * 1024)
// menu lines a minute at most; the minute's line counts the rest
#define Crash_Log_Menu_Lines 40

// ---- the game's side, in the order dylib_main calls it
// Before the log is opened. keep: KeepCrashLogs. folder: BO3MacFix's. to_file: this start writes log.txt (started from
// Steam). Judges the run before by its log, keeps its log, and returns the path to open the log at (in
// BO3MACFIX_CRASHLOGDIR's folder for checks).
const char *crash_log_prepare(int keep, const char *folder, bool to_file);
// Once the log is open: the start's lines and what became of the run before, then the thread that looks for macOS's
// report and tells the time once a minute. base_address: the game's, for its frame count.
void crash_log_started(uint64_t base_address, const char *settings_path);
// after notices_init: one notice when the run before didn't end by quitting
void crash_log_notice(void);
// exit() (qol.c), on any thread, before anything else it does: the line that says the game ended by quitting
void crash_log_quit(int status);
// HKAI_InitMapData: a map loads, core_frontend for the menus
void crash_log_map(const char *map);
// notices.m's lua_load_ui, when the first menu of a UI Lua state is made: crash_log.lua, the menus' lines
void crash_log_lua_install(lua_State *s);

// ---- plain C, for tests/test_crash_log.c
// when a run started, as its log's first line says
typedef struct {
    int year, month, day, hour, minute, second;
    int zone_minutes; // ahead of UTC then
    int pid;
} crash_log_start_t;

// the first line of a log, without its line break: "BO3MacFix: log of the start on 2026-11-18 21:03:05 +1100, pid 812"
void crash_log_format_start(const crash_log_start_t *start, char *out, size_t size);
// finds that line at the start of a line within text[0..length) and reads it back; false if there is none
bool crash_log_parse_start(const char *text, size_t length, crash_log_start_t *start);
// that start in seconds since 1970
long long crash_log_start_epoch(const crash_log_start_t *start);
// the crash log's name for a run that started then: crash-20261118-210305.txt
void crash_log_crash_name(const crash_log_start_t *start, char *out, size_t size);
// whether name is a crash log's: "crash-", 8 digits, "-", 6 digits, ".txt", nothing else
bool crash_log_is_crash_name(const char *name);
// whether the quit line is at the start of a line in the end of a log, tail[0..length); from_file_start: tail is the
// whole log, so its first line counts too
bool crash_log_quit_in_tail(const char *tail, size_t length, bool from_file_start);

typedef enum {
    Previous_None,    // no log.txt: the first start that writes one
    Previous_Unknown, // a log without the first line: from a build without KeepCrashLogs
    Previous_Quit,    // it ended by quitting
    Previous_Running, // its game is still running: two copies of the game
    Previous_Crashed, // it didn't end by quitting: a crash, a force quit, a power cut
} crash_log_previous_t;

typedef struct {
    crash_log_previous_t previous;
    crash_log_start_t start; // its first line, for Previous_Quit, Previous_Running and Previous_Crashed
    char crash_name[48];     // Previous_Crashed: the crash log it is kept as, "" when it couldn't be
    bool moved;              // log.txt is log-previous.txt now
    int removed;             // crash logs removed past keep
} crash_log_kept_t;

// In folder: judges log.txt, makes it log-previous.txt, keeps it as a crash log too (a second name for the same file,
// or a copy where the disk has no second names) when its run didn't end by quitting, and removes the oldest crash logs
// past keep. Touches no file but log.txt, log-previous.txt and crash logs. running(pid): that pid is the game, running.
void crash_log_keep(const char *folder, int keep, bool (*running)(int pid), crash_log_kept_t *kept);
// removes the crash logs in folder past the newest keep (the names sort by time); how many it removed
int crash_log_prune(const char *folder, int keep);
// copies the file at from to a new file at to (not over one that is there), at most limit bytes
bool crash_log_copy(const char *from, const char *to, long long limit);

// what the game is doing for the time lines: "starting" before a map, "in the menus", "in a game on zm_factory"
void crash_log_state_text(const char *map, char *out, size_t size);
// "time 21:04:05, 65.3 s since the start, frame 7234, in the menus" (no frame while it is 0)
void crash_log_format_time(const struct tm *local, double since_start, uint64_t frame, const char *map, char *out,
                           size_t size);

// Crash_Log_Menu_Lines a minute, the rest counted in left_out
typedef struct {
    double window_start;
    int lines, left_out;
} crash_log_limit_t;
bool crash_log_limit(crash_log_limit_t *limit, double now);

typedef enum {
    Report_None,       // no report of that run
    Report_Summarised, // summary holds its summary
    Report_Unreadable, // one of that time that couldn't be read whole; summary says why
} crash_log_report_t;

// macOS's report of a run: in folders, files named CoDBlkOps3_Exe*.ips written at since (seconds since 1970) or later,
// the one of pid whose procPath is one of executables (crash_report_summary). Its path into path, its summary into
// summary.
crash_log_report_t crash_log_find_report(const char *const folders[], int count, long long since, int pid,
                                         const char *const *executables, char *path, size_t path_size, char *summary,
                                         size_t summary_size);
