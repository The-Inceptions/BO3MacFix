// Reading one of macOS's crash reports of the game (an .ips file) for KeepCrashLogs (crash_log.c): a short summary of
// how and where the game crashed, for the log of that run. The reader runs inside the game, at the start after a
// crash, on a file anybody could have put there, so it trusts nothing in it: it reads at most Crash_Report_Max_Size
// bytes, follows nesting at most Crash_Report_Max_Depth deep, checks every read against the end, and on any doubt says
// only that it couldn't read the file. Plain C without game or macOS calls: tests/test_crash_report.c runs it on real
// reports, on every cut of them, on random bytes and on deeply nested ones.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// the game's reports are 50 to 100 KB; one with hundreds of threads stays far below this
#define Crash_Report_Max_Size (4 << 20)
// a report nests 5 deep (the body, its threads, a thread's state, a register)
#define Crash_Report_Max_Depth 32
// frames of the crashed thread and of the game's "Main" thread in a summary
#define Crash_Report_Frames 16
#define Crash_Report_Main_Frames 8

typedef enum {
    Crash_Report_Summarised,    // out holds the summary
    Crash_Report_Other_Process, // a whole report, but of another process (another pid, or a program elsewhere)
    Crash_Report_Unreadable,    // not a report this reader can read whole (cut off, damaged, another kind): out says why
} crash_report_result_t;

// Summarises the report text[0..length): the process and its times, the exception, the crashed thread's name, rip and
// first Crash_Report_Frames frames, and the first Crash_Report_Main_Frames frames of the game's "Main" thread when
// another thread crashed; lines that end in '\n'. Frames inside the game are full addresses (image base 0x100000000,
// as in wip/listing/mac.asm), inside BO3MacFix.dylib its offset and the symbol macOS gave, elsewhere the library and
// symbol. pid: the process the report must be of, 0 for any. executables: the programs (real paths, a list that ends
// in NULL) one of which the report's procPath must name, so that a copy of the game elsewhere or a stand-in by its
// name is never taken for it; NULL for any. out is always a string, cut to fit.
crash_report_result_t crash_report_summary(const char *text, size_t length, int pid, const char *const *executables,
                                           char *out, size_t out_size);

// Whether path (a program's real path) is the one a report's procPath names. macOS writes the user's home folder as
// /Users/USER and may put a "*" in place of any part of the path (the game's reports say ".../Steam/*/CoDBlkOps3.app/
// ..." for steamapps/common/Call of Duty Black Ops III), so "*" stands for any text there.
bool crash_report_path_matches(const char *report_path, const char *path);

// Reads the file at path into a new buffer that ends in a 0 (free it), length set to its bytes. NULL, with why set,
// if it isn't a regular file (a link, a pipe...), is empty or bigger than Crash_Report_Max_Size, or can't be read.
char *crash_report_read(const char *path, size_t *length, const char **why);
