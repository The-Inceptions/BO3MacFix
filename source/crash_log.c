// See crash_log.h.
//
// The start: crash_log_prepare looks at the log.txt the run before left, before the new log is opened over it. The
// log's first line says when that run started (date, time, zone and pid); a line among its last 256 KB says how it
// ended. Every orderly way out of the game ends in exit(), which qol.c replaces for the whole process, and that writes
// the line first, on whatever thread and whatever ExitFix says, with the exit status: QUIT in the menus, the mod's quit
// command, Cmd+Q (SDL only queues a quit for the game's own path) and the settings page's restart (a "quit" command)
// exit with 0 (Crash_Log_Quit_Line); a fatal Com_Error (Sys_Error ends in Aspyr's exit function, exit(-1)) with -1
// (Crash_Log_Error_Line). A crash, a force quit and a power cut never get there. A log without the line, or with a
// status other than 0, gets a second name in the same folder, crash-<its start>.txt, after it became
// log-previous.txt, and the oldest crash logs go (past KeepCrashLogs, or past Crash_Logs_Max_Total together, by their
// files' own time). The second name is a hard link, so nothing is copied; on a disk without hard links it is a copy.
// What is kept of a log is its end: a log bigger than Crash_Log_Max_Size is copied as its last 16 MB.
//
// The lines: crash_log_started writes the start's lines at the top of the new log. A thread of ours at utility QoS
// ("BO3MacFix crash log") then looks for macOS's report of the run before, at once and, if it isn't there yet, once
// more a minute later (macOS takes a moment to write it, and the user's game started again 12 seconds after a crash),
// adds its summary to the crash log, and writes the time line once a minute. Menus (crash_log.lua through notices.m),
// maps (HKAI_InitMapData) and quitting get a line with the time when they happen, through printf from the game's own
// thread like every other line of BO3MacFix: stdout stays a line-buffered file the game writes into directly. The
// thread's own lines go to the file with one write() each, so it never holds stdout's lock while it waits for the disk.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include "crash_log.h"
#include "crash_log_lua.h"
#include "crash_report.h"
#include "notices.h"
#include "offsets.h"
#include "versioninfo.h"
// the commit the library was built from, which the Makefile writes; without it, the time it was compiled
#if __has_include("build_name.h")
#include "build_name.h"
#endif
#ifndef MACFIX_BUILD
#define MACFIX_BUILD ""
#endif

#define Max_Keep 1000                // crash logs kept at most, whatever the setting says
#define Max_Crash_Logs 4096          // crash logs looked at when old ones are removed
#define Max_Report_Entries 100000    // names looked at in a report folder
#define Report_Clock_Slack 5         // seconds a report's time may be before the start (clocks in whole seconds)
#define Crash_Name_Size 32

#pragma mark - plain C

void crash_log_format_start(const crash_log_start_t *start, char *out, size_t size) {
    int zone = start->zone_minutes < 0 ? -start->zone_minutes : start->zone_minutes;
    snprintf(out, size, Crash_Log_Start_Line "%04d-%02d-%02d %02d:%02d:%02d %c%02d%02d, pid %d", start->year,
             start->month, start->day, start->hour, start->minute, start->second, start->zone_minutes < 0 ? '-' : '+',
             zone / 60, zone % 60, start->pid);
}

static bool valid_start(const crash_log_start_t *s) {
    return s->year >= 1970 && s->year <= 9999 && s->month >= 1 && s->month <= 12 && s->day >= 1 && s->day <= 31 &&
           s->hour >= 0 && s->hour <= 23 && s->minute >= 0 && s->minute <= 59 && s->second >= 0 && s->second <= 60 &&
           s->zone_minutes >= -16 * 60 && s->zone_minutes <= 16 * 60 && s->pid > 0;
}

// reads the rest of the first line after Crash_Log_Start_Line; anything after the pid must start with a space
static bool parse_start_rest(const char *rest, crash_log_start_t *start) {
    crash_log_start_t s;
    char sign = 0;
    int zone_hours = -1, zone_minutes = -1, used = 0;
    if (sscanf(rest, "%4d-%2d-%2d %2d:%2d:%2d %c%2d%2d, pid %d%n", &s.year, &s.month, &s.day, &s.hour, &s.minute,
               &s.second, &sign, &zone_hours, &zone_minutes, &s.pid, &used) != 10 ||
        (rest[used] != '\0' && rest[used] != ' ') || (sign != '+' && sign != '-') || zone_hours < 0 ||
        zone_hours > 16 || zone_minutes < 0 || zone_minutes > 59)
        return false;
    // the fields are fixed width: "2026-11-18 21:03:05 +1100" is 25 characters
    if (strncmp(rest + 4, "-", 1) != 0 || strncmp(rest + 10, " ", 1) != 0 || strncmp(rest + 19, " ", 1) != 0)
        return false;
    s.zone_minutes = (sign == '-' ? -1 : 1) * (zone_hours * 60 + zone_minutes);
    if (!valid_start(&s))
        return false;
    *start = s;
    return true;
}

bool crash_log_parse_start(const char *text, size_t length, crash_log_start_t *start) {
    size_t prefix = strlen(Crash_Log_Start_Line);
    size_t at = 0;
    while (text != NULL && at < length) {
        const char *line = text + at;
        const char *end = memchr(line, '\n', length - at);
        size_t line_length = end != NULL ? (size_t)(end - line) : length - at;
        char rest[160];
        if (line_length > prefix && line_length - prefix < sizeof(rest) &&
            memcmp(line, Crash_Log_Start_Line, prefix) == 0) {
            memcpy(rest, line + prefix, line_length - prefix);
            rest[line_length - prefix] = '\0';
            if (parse_start_rest(rest, start))
                return true;
        }
        if (end == NULL)
            break;
        at += line_length + 1;
    }
    return false;
}

long long crash_log_start_epoch(const crash_log_start_t *start) {
    struct tm utc = { .tm_year = start->year - 1900, .tm_mon = start->month - 1, .tm_mday = start->day,
                      .tm_hour = start->hour, .tm_min = start->minute, .tm_sec = start->second };
    return (long long)timegm(&utc) - start->zone_minutes * 60LL;
}

void crash_log_crash_name(const crash_log_start_t *start, char *out, size_t size) {
    snprintf(out, size, "crash-%04d%02d%02d-%02d%02d%02d.txt", start->year, start->month, start->day, start->hour,
             start->minute, start->second);
}

bool crash_log_is_crash_name(const char *name) {
    static const char pattern[] = "crash-########-######.txt";
    for (size_t i = 0; i < sizeof(pattern) - 1; i++) {
        char c = name[i];
        if (pattern[i] == '#' ? c < '0' || c > '9' : c != pattern[i])
            return false;
    }
    return name[sizeof(pattern) - 1] == '\0';
}

// whether text[at..length) starts with marker
static bool starts_with(const char *text, size_t length, size_t at, const char *marker) {
    size_t marker_length = strlen(marker);
    return at <= length && length - at >= marker_length && memcmp(text + at, marker, marker_length) == 0;
}

bool crash_log_end_in_tail(const char *tail, size_t length, bool from_file_start, int *status) {
    if (tail == NULL)
        return false;
    // the last line that starts with either; a line starts after a line break, and at 0 only in the whole log
    size_t found = SIZE_MAX;
    bool error = false;
    for (size_t at = 0; at < length;) {
        if (at > 0 || from_file_start) {
            if (starts_with(tail, length, at, Crash_Log_Quit_Line))
                found = at, error = false;
            else if (starts_with(tail, length, at, Crash_Log_Error_Line))
                found = at, error = true;
        }
        const char *next = memchr(tail + at, '\n', length - at);
        if (next == NULL)
            break;
        at = (size_t)(next - tail) + 1;
    }
    if (found == SIZE_MAX)
        return false;
    // its status, ", exit status <n>" on that line
    char line[200];
    size_t line_length = length - found;
    const char *line_end = memchr(tail + found, '\n', line_length);
    if (line_end != NULL)
        line_length = (size_t)(line_end - (tail + found));
    if (line_length >= sizeof(line))
        line_length = sizeof(line) - 1;
    memcpy(line, tail + found, line_length);
    line[line_length] = '\0';
    const char *at = strstr(line, ", exit status ");
    char *end = NULL;
    long value = at != NULL ? strtol(at + strlen(", exit status "), &end, 10) : 0;
    bool read = at != NULL && end != at + strlen(", exit status ") && value >= INT_MIN && value <= INT_MAX;
    *status = read ? (int)value : error ? INT_MIN : 0;
    return true;
}

// Crash_Log_Max_Size and Crash_Logs_Max_Total, which tests make smaller
static long long kept_log_limit = Crash_Log_Max_Size, crash_logs_limit = Crash_Logs_Max_Total;

static void path_in(char *out, size_t size, const char *folder, const char *name) {
    snprintf(out, size, "%s/%s", folder, name);
}

// up to size bytes of fd from offset; how many there were
static size_t read_at(int fd, char *buffer, size_t size, off_t offset) {
    size_t got = 0;
    while (got < size) {
        ssize_t r = pread(fd, buffer + got, size - got, offset + (off_t)got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    return got;
}

static bool write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t w = write(fd, data, length);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        data += w;
        length -= (size_t)w;
    }
    return true;
}

long long crash_log_copy(const char *from, const char *to, long long limit) {
    int in = open(from, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (in < 0)
        return -1;
    struct stat st;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(in);
        return -1;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    enum { Chunk = 1 << 16 };
    char *buffer = malloc(Chunk);
    bool copied = buffer != NULL;
    // what is copied: all of it, or the last limit bytes from the first line's start among them (the end is what a
    // crash log is for); never past the size it had, which the game may still be adding to
    long long size = st.st_size, start = 0;
    if (copied && limit >= 0 && size > limit) {
        start = size - limit;
        ssize_t r = pread(in, buffer, Chunk, start);
        const char *line_end = r > 0 ? memchr(buffer, '\n', (size_t)r) : NULL;
        if (line_end != NULL)
            start += line_end - buffer + 1;
        char first[200];
        int n = snprintf(first, sizeof(first), "BO3MacFix: crash logs: the first %lld bytes of this log (%.1f MB) were "
                         "left out to keep its size down, its last %lld follow\n", start, (double)start / (1 << 20),
                         size - start);
        copied = n > 0 && write_all(out, first, (size_t)n);
    }
    for (long long done = start; copied && done < size;) {
        size_t want = size - done < Chunk ? (size_t)(size - done) : Chunk;
        ssize_t r = pread(in, buffer, want, done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r == 0)
            break;
        copied = r > 0 && write_all(out, buffer, (size_t)r);
        done += r;
    }
    // the copy has the log's own time, by which crash logs are kept and removed
    struct timespec times[2] = { st.st_atimespec, st.st_mtimespec };
    if (copied)
        futimens(out, times);
    free(buffer);
    close(in);
    if (close(out) != 0)
        copied = false;
    if (!copied) {
        unlink(to);
        return -1;
    }
    return start;
}

typedef struct {
    char name[Crash_Name_Size];
    struct timespec time; // the file's own (a link's own, not what it points to)
    long long size;
} crash_file_t;

// newest first by the file's time; the name decides between two of one time
static int newest_first(const void *a, const void *b) {
    const crash_file_t *x = a, *y = b;
    if (x->time.tv_sec != y->time.tv_sec)
        return x->time.tv_sec < y->time.tv_sec ? 1 : -1;
    if (x->time.tv_nsec != y->time.tv_nsec)
        return x->time.tv_nsec < y->time.tv_nsec ? 1 : -1;
    return strcmp(y->name, x->name);
}

int crash_log_prune(const char *folder, int keep, long long total, const char *stays) {
    if (keep <= 0)
        return 0;
    DIR *dir = opendir(folder);
    if (dir == NULL)
        return 0;
    crash_file_t *files = malloc(sizeof(*files) * Max_Crash_Logs);
    int count = 0;
    struct dirent *entry;
    while (files != NULL && count < Max_Crash_Logs && (entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        path_in(path, sizeof(path), folder, entry->d_name);
        // a file or a link by a crash log's name (a link goes, not what it points to); never a folder
        if (!crash_log_is_crash_name(entry->d_name) || lstat(path, &st) != 0 ||
            !(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)))
            continue;
        snprintf(files[count].name, Crash_Name_Size, "%s", entry->d_name);
        files[count].time = st.st_mtimespec;
        files[count].size = st.st_size;
        count++;
    }
    closedir(dir);
    if (files == NULL)
        return 0;
    qsort(files, (size_t)count, sizeof(*files), newest_first);
    // the one this start kept goes first, whatever its time
    for (int i = 1; stays != NULL && i < count; i++) {
        if (strcmp(files[i].name, stays) == 0) {
            crash_file_t first = files[i];
            memmove(files + 1, files, sizeof(*files) * (size_t)i);
            files[0] = first;
            break;
        }
    }
    // the first stay while there are no more than keep of them and they come to no more than total; the rest go
    int removed = 0;
    long long kept_bytes = 0;
    bool full = false;
    for (int i = 0; i < count; i++) {
        if (!full && i < keep && (i == 0 || kept_bytes + files[i].size <= total)) {
            kept_bytes += files[i].size;
            continue;
        }
        full = true;
        char path[PATH_MAX];
        path_in(path, sizeof(path), folder, files[i].name);
        if (unlink(path) == 0)
            removed++;
    }
    free(files);
    return removed;
}

void crash_log_keep(const char *folder, int keep, bool (*running)(int pid), crash_log_kept_t *kept) {
    memset(kept, 0, sizeof(*kept));
    char log_path[PATH_MAX], previous_path[PATH_MAX];
    path_in(log_path, sizeof(log_path), folder, "log.txt");
    path_in(previous_path, sizeof(previous_path), folder, "log-previous.txt");
    int fd = open(log_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        kept->previous = Previous_None;
        kept->removed = crash_log_prune(folder, keep, crash_logs_limit, NULL);
        return;
    }
    // its first line, and its end
    char *head = malloc(Crash_Log_Head), *tail = malloc(Crash_Log_Tail);
    size_t head_length = 0, tail_length = 0;
    bool whole = st.st_size <= Crash_Log_Tail;
    if (head != NULL && tail != NULL) {
        head_length = read_at(fd, head, Crash_Log_Head, 0);
        tail_length = read_at(fd, tail, Crash_Log_Tail, whole ? 0 : st.st_size - Crash_Log_Tail);
    }
    close(fd);
    int status = 0;
    if (!crash_log_parse_start(head, head_length, &kept->start))
        kept->previous = Previous_Unknown;
    else if (crash_log_end_in_tail(tail, tail_length, whole, &status))
        kept->previous = status == 0 ? Previous_Quit : Previous_Error;
    else if (running != NULL && running(kept->start.pid))
        kept->previous = Previous_Running;
    else
        kept->previous = Previous_Crashed;
    kept->status = status;
    free(head);
    free(tail);
    // out of the way first: the new log opens log.txt for writing and empties it, with every name it has. A log too big
    // to keep whole leaves its end in log-previous.txt (and then goes), or, when that can't be written, all of it.
    if (st.st_size <= kept_log_limit) {
        kept->moved = rename(log_path, previous_path) == 0;
    } else {
        struct stat previous_st;
        if (lstat(previous_path, &previous_st) == 0 && (S_ISREG(previous_st.st_mode) || S_ISLNK(previous_st.st_mode)))
            unlink(previous_path);
        long long left_out = crash_log_copy(log_path, previous_path, kept_log_limit);
        if (left_out >= 0) {
            kept->moved = true;
            kept->left_out = left_out;
            unlink(log_path);
        } else {
            kept->moved = rename(log_path, previous_path) == 0;
        }
    }
    if (kept->previous == Previous_Crashed || kept->previous == Previous_Error) {
        char name[Crash_Name_Size], crash_path[PATH_MAX];
        struct stat existing;
        crash_log_crash_name(&kept->start, name, sizeof(name));
        path_in(crash_path, sizeof(crash_path), folder, name);
        // a second name for log-previous.txt; one that is there already is that run's, kept before
        bool there = kept->moved && link(previous_path, crash_path) == 0;
        if (!there && lstat(crash_path, &existing) == 0) {
            there = true;
        } else if (!there) {
            long long left_out = crash_log_copy(kept->moved ? previous_path : log_path, crash_path, kept_log_limit);
            there = left_out >= 0;
            if (left_out > 0)
                kept->left_out = left_out;
        }
        if (there)
            snprintf(kept->crash_name, sizeof(kept->crash_name), "%s", name);
    }
    kept->removed = crash_log_prune(folder, keep, crash_logs_limit, kept->crash_name[0] != '\0' ? kept->crash_name : NULL);
}

void crash_log_state_text(const char *map, char *out, size_t size) {
    if (map == NULL || map[0] == '\0')
        snprintf(out, size, "starting");
    else if (strcmp(map, "core_frontend") == 0)
        snprintf(out, size, "in the menus");
    else
        snprintf(out, size, "in a game on %s", map);
}

void crash_log_format_time(const struct tm *local, double since_start, uint64_t frame, const char *map, char *out,
                           size_t size) {
    char state_text[96], frame_text[32] = "";
    crash_log_state_text(map, state_text, sizeof(state_text));
    if (frame != 0)
        snprintf(frame_text, sizeof(frame_text), ", frame %" PRIu64, frame);
    snprintf(out, size, "time %02d:%02d:%02d, %.1f s since the start%s, %s", local->tm_hour, local->tm_min,
             local->tm_sec, since_start, frame_text, state_text);
}

bool crash_log_limit(crash_log_limit_t *limit, double now) {
    if (now < limit->window_start || now - limit->window_start >= 60.0) {
        limit->window_start = now;
        limit->lines = 0;
    }
    if (limit->lines >= Crash_Log_Menu_Lines) {
        limit->left_out++;
        return false;
    }
    limit->lines++;
    return true;
}

crash_log_report_t crash_log_find_report(const char *const folders[], int count, long long since, int pid,
                                         const char *const *executables, char *path, size_t path_size, char *summary,
                                         size_t summary_size) {
    char candidate[PATH_MAX] = "", why[256] = "";
    for (int f = 0; f < count; f++) {
        if (folders[f] == NULL)
            continue;
        DIR *dir = opendir(folders[f]);
        if (dir == NULL)
            continue;
        struct dirent *entry;
        for (int scanned = 0; scanned < Max_Report_Entries && (entry = readdir(dir)) != NULL; scanned++) {
            size_t length = strlen(entry->d_name);
            if (strncmp(entry->d_name, "CoDBlkOps3_Exe", 14) != 0 || length < 18 ||
                strcmp(entry->d_name + length - 4, ".ips") != 0)
                continue;
            char full[PATH_MAX];
            struct stat st;
            if ((size_t)snprintf(full, sizeof(full), "%s/%s", folders[f], entry->d_name) >= sizeof(full) ||
                lstat(full, &st) != 0 || !S_ISREG(st.st_mode) || (long long)st.st_mtime < since - Report_Clock_Slack)
                continue;
            size_t text_length;
            const char *not_read;
            char *text = crash_report_read(full, &text_length, &not_read);
            crash_report_result_t result = Crash_Report_Unreadable;
            if (text != NULL) {
                result = crash_report_summary(text, text_length, pid, executables, summary, summary_size);
                free(text);
            } else {
                snprintf(summary, summary_size, "%s\n", not_read);
            }
            if (result == Crash_Report_Summarised) {
                closedir(dir);
                snprintf(path, path_size, "%s", full);
                return Report_Summarised;
            }
            // one of that time that couldn't be read whole may be the one: named if no other is
            if (result == Crash_Report_Unreadable && candidate[0] == '\0') {
                snprintf(candidate, sizeof(candidate), "%s", full);
                snprintf(why, sizeof(why), "%s", summary);
            }
        }
        closedir(dir);
    }
    if (summary_size > 0)
        summary[0] = '\0';
    if (candidate[0] == '\0')
        return Report_None;
    snprintf(path, path_size, "%s", candidate);
    snprintf(summary, summary_size, "%s", why);
    return Report_Unreadable;
}

#pragma mark - the game

static struct {
    bool enabled;  // KeepCrashLogs > 0: the lines
    bool rotating; // and this start looked at the log before: it writes log.txt, or the checks' folder
    bool test, to_file, debug;
    int keep;
    char folder[PATH_MAX];
    crash_log_kept_t kept;
    uint64_t base_address;
    int log_fd; // where stdout writes once the log is open (freopen takes the lowest free number, not always 1)
    double started; // now() at the start
    atomic_bool quit_written;
} state = { .log_fd = STDOUT_FILENO };

static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
static char map_name[64];             // under lock: the map loaded last
static char newest_menu[64];          // under lock: the newest menu still open, as crash_log.lua last said
static crash_log_limit_t menu_limit;  // under lock
static double clock_interval = 60.0;  // seconds between time lines, and before the second look for the report
static const char *report_folders[2]; // where macOS puts the reports: the system's and the user's
static char user_report_folder[PATH_MAX];
// the game's two programs in the folder of the one running now (the pre-patched copy and the stock one, either of
// which the run before may have been), so that only their reports are taken, never a copy's elsewhere; empty when the
// path can't be found
static char game_programs[2][PATH_MAX];
static const char *game_executables[3];
// what a running copy of the game is called (a test, which isn't the game, sets what it is called itself)
static const char *game_name = "CoDBlkOps3_Exe";

static double now(void) {
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e9;
}

static uint64_t frame_count(void) {
    return state.base_address != 0 ? *(volatile uint64_t *)(state.base_address + ADDR_rt_frame_counter) : 0;
}

// "time 21:04:05, 65.3 s since the start, frame 7234, in the menus" for now
static void time_now(char *out, size_t size) {
    time_t t = time(NULL);
    struct tm local;
    localtime_r(&t, &local);
    char map[sizeof(map_name)];
    os_unfair_lock_lock(&lock);
    memcpy(map, map_name, sizeof(map));
    os_unfair_lock_unlock(&lock);
    crash_log_format_time(&local, now() - state.started, frame_count(), map, out, size);
}

// a line from the thread: one write() straight into the log, past stdout's buffer and lock
__attribute__((format(printf, 1, 2))) static void thread_line(const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (n < 0)
        return;
    size_t length = (size_t)n < sizeof(line) - 2 ? (size_t)n : sizeof(line) - 2;
    line[length++] = '\n';
    write_all(state.log_fd, line, length);
}

// whether pid is a copy of the game that is running now (not this one: a pid can come back after a restart)
static bool game_running(int pid) {
    char name[64] = "";
    return pid > 0 && pid != getpid() && proc_name(pid, name, sizeof(name)) > 0 &&
           strncmp(name, game_name, strlen(game_name)) == 0;
}

// the real paths of the game's two programs next to the running one
static void find_game_programs(void) {
    char path[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0 || realpath(path, real) == NULL)
        return;
    char *slash = strrchr(real, '/');
    if (slash == NULL)
        return;
    *slash = '\0';
    snprintf(game_programs[0], sizeof(game_programs[0]), "%s/CoDBlkOps3_Exe", real);
    snprintf(game_programs[1], sizeof(game_programs[1]), "%s/CoDBlkOps3_Exe.bo3macfix", real);
    game_executables[0] = game_programs[0];
    game_executables[1] = game_programs[1];
}

static void macos_text(char *out, size_t size) {
    char version[32] = "?", build[32] = "?";
    size_t length = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &length, NULL, 0) != 0)
        snprintf(version, sizeof(version), "?");
    length = sizeof(build);
    if (sysctlbyname("kern.osversion", build, &length, NULL, 0) != 0)
        snprintf(build, sizeof(build), "?");
    snprintf(out, size, "macOS %s (%s)", version, build);
}

// "on battery (87%)", "on mains power (battery 87%)", "on mains power"
static void power_text(char *out, size_t size) {
    snprintf(out, size, "power unknown");
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == NULL)
        return;
    CFStringRef source = IOPSGetProvidingPowerSourceType(info);
    bool battery = source != NULL && CFStringCompare(source, CFSTR(kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo;
    int percent = -1;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; list != NULL && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef description = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        CFStringRef type = description != NULL ? CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey)) : NULL;
        CFNumberRef current = description != NULL ? CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey))
                                                  : NULL;
        CFNumberRef max = description != NULL ? CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey)) : NULL;
        int now_capacity = 0, max_capacity = 0;
        if (type != NULL && CFGetTypeID(type) == CFStringGetTypeID() &&
            CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) == kCFCompareEqualTo && current != NULL &&
            max != NULL && CFGetTypeID(current) == CFNumberGetTypeID() && CFGetTypeID(max) == CFNumberGetTypeID() &&
            CFNumberGetValue(current, kCFNumberIntType, &now_capacity) &&
            CFNumberGetValue(max, kCFNumberIntType, &max_capacity) && max_capacity > 0)
            percent = now_capacity * 100 / max_capacity;
    }
    if (list != NULL)
        CFRelease(list);
    CFRelease(info);
    if (battery && percent >= 0)
        snprintf(out, size, "on battery (%d%%)", percent);
    else if (battery)
        snprintf(out, size, "on battery");
    else if (percent >= 0)
        snprintf(out, size, "on mains power (battery %d%%)", percent);
    else
        snprintf(out, size, "on mains power");
}

// whether settings.ini was there to be read, and how many lines in it set something
static void settings_text(const char *path, char *out, size_t size) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        snprintf(out, size, "no settings.ini (every setting at its default)");
        return;
    }
    int count = 0;
    char line[1024];
    bool line_start = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        const char *p = line + strspn(line, " \t");
        if (line_start && *p != '/' && *p != '#' && *p != ';' && strchr(p, '=') != NULL)
            count++;
        size_t length = strlen(line);
        line_start = length > 0 && line[length - 1] == '\n';
    }
    fclose(file);
    snprintf(out, size, "settings.ini read (%d settings in it)", count);
}

const char *crash_log_prepare(int keep, const char *folder, bool to_file) {
    static char log_path[PATH_MAX];
    path_in(log_path, sizeof(log_path), folder, "log.txt");
    if (keep <= 0)
        return log_path;
    const char *check_folder = getenv("BO3MACFIX_CRASHLOGDIR");
    const char *debug = getenv("BO3MACFIX_CRASHLOGDEBUG");
    state.enabled = true;
    state.started = now();
    state.debug = debug != NULL && atoi(debug) != 0;
    state.keep = keep > Max_Keep ? Max_Keep : keep;
    state.to_file = to_file;
    state.test = check_folder != NULL && check_folder[0] != '\0';
    snprintf(state.folder, sizeof(state.folder), "%s", state.test ? check_folder : folder);
    if (state.test) {
        mkdir(state.folder, 0755);
        path_in(log_path, sizeof(log_path), state.folder, "log.txt");
    }
    // started from a terminal or by the test runner, the log goes elsewhere and log.txt stays as it is
    state.rotating = to_file || state.test;
    if (state.rotating)
        crash_log_keep(state.folder, state.keep, game_running, &state.kept);
    return log_path;
}

static void say_previous(void) {
    const crash_log_kept_t *kept = &state.kept;
    char started[64], cut[96] = "", how[96];
    snprintf(started, sizeof(started), "%04d-%02d-%02d %02d:%02d:%02d", kept->start.year, kept->start.month,
             kept->start.day, kept->start.hour, kept->start.minute, kept->start.second);
    // what is kept of a log too big to keep whole: its end
    if (kept->left_out > 0)
        snprintf(cut, sizeof(cut), " (its end: the first %.1f MB were left out)", (double)kept->left_out / (1 << 20));
    // a log that couldn't be moved out of the way (a folder that can't be written) is written over by this start
    const char *lost = "it couldn't be moved to log-previous.txt, so this start writes over it";
    switch (kept->previous) {
    case Previous_None:
        printf("BO3MacFix: crash logs: no log of a run before in %s\n", state.folder);
        break;
    case Previous_Unknown:
        if (kept->moved)
            printf("BO3MacFix: crash logs: the log before is from a build that doesn't say how it ended; it is "
                   "log-previous.txt now%s\n", cut);
        else
            printf("BO3MacFix: crash logs: the log before is from a build that doesn't say how it ended; %s\n", lost);
        break;
    case Previous_Quit:
        if (kept->moved)
            printf("BO3MacFix: crash logs: the run before (started %s) ended by quitting; its log is "
                   "log-previous.txt%s\n", started, cut);
        else
            printf("BO3MacFix: crash logs: the run before (started %s) ended by quitting; %s\n", started, lost);
        break;
    case Previous_Running:
        if (kept->moved)
            printf("BO3MacFix: crash logs: the run before (started %s, pid %d) is still running; its log is "
                   "log-previous.txt now%s\n", started, kept->start.pid, cut);
        else
            printf("BO3MacFix: crash logs: the run before (started %s, pid %d) is still running; %s\n", started,
                   kept->start.pid, lost);
        break;
    case Previous_Crashed:
    case Previous_Error:
        if (kept->previous == Previous_Crashed)
            snprintf(how, sizeof(how), "didn't end by quitting (a crash, a force quit or a power cut)");
        else if (kept->status == INT_MIN)
            snprintf(how, sizeof(how), "ended with an error");
        else
            snprintf(how, sizeof(how), "ended with an error (exit status %d)", kept->status);
        if (kept->crash_name[0] != '\0')
            printf("BO3MacFix: crash logs: the run before (started %s, pid %d) %s; its log is kept as %s%s, macOS's "
                   "crash report of it is looked for\n", started, kept->start.pid, how, kept->crash_name, cut);
        else if (kept->moved)
            printf("BO3MacFix: crash logs: the run before (started %s, pid %d) %s, and its log couldn't be kept "
                   "apart; it is log-previous.txt%s\n", started, kept->start.pid, how, cut);
        else
            printf("BO3MacFix: crash logs: the run before (started %s, pid %d) %s, and its log couldn't be kept: this "
                   "start writes over it\n", started, kept->start.pid, how);
        break;
    }
    if (kept->removed > 0)
        printf("BO3MacFix: crash logs: removed the %d oldest, keeping the newest %d and %lld MB at most "
               "(KeepCrashLogs)\n", kept->removed, state.keep, crash_logs_limit >> 20);
}

// for checks (BO3MACFIX_CRASHLOGDIR) started by the test runner, whose own file is the output: that file becomes the
// folder's log.txt as well, so the next run finds it there as after a start from Steam, and the runner keeps its log
static void link_output(void) {
    char output[PATH_MAX], log_path[PATH_MAX];
    path_in(log_path, sizeof(log_path), state.folder, "log.txt");
    unlink(log_path);
    if (fcntl(state.log_fd, F_GETPATH, output) == -1) {
        printf("BO3MacFix: crash logs: can't tell where the output goes, so %s isn't this run's\n", log_path);
        return;
    }
    if (link(output, log_path) != 0 && symlink(output, log_path) != 0)
        printf("BO3MacFix: crash logs: couldn't give %s the name %s: %s\n", output, log_path, strerror(errno));
    else
        printf("BO3MacFix: crash logs: checks' folder %s (BO3MACFIX_CRASHLOGDIR), this run's log is its log.txt\n",
               state.folder);
}

// the exception's and the crashed thread's lines of a summary, for the log's one line about the report
static void summary_line(const char *summary, const char *start, char *out, size_t size) {
    const char *line = strstr(summary, start);
    size_t length = line != NULL ? strcspn(line, "\n") : 0;
    snprintf(out, size, "%.*s", (int)length, line != NULL ? line : "");
}

// adds text to the end of the run before's crash log
static void add_to_crash_log(const char *text) {
    char path[PATH_MAX];
    path_in(path, sizeof(path), state.folder, state.kept.crash_name);
    int fd = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return;
    write_all(fd, text, strlen(text));
    close(fd);
}

// looks for macOS's report of the run before; only the last look settles for none, or for one that can't be read
// whole (macOS may still be writing it at the first). Whether there's no need to look again.
static bool look_for_report(bool last) {
    static char path[PATH_MAX], summary[8192], text[9000];
    const crash_log_start_t *start = &state.kept.start;
    crash_log_report_t found = crash_log_find_report(report_folders, 2, crash_log_start_epoch(start), start->pid,
                                                     game_executables, path, sizeof(path), summary, sizeof(summary));
    if (state.debug)
        thread_line("BO3MacFix: crash logs: looked for the report of pid %d in %s and %s: %s", start->pid,
                    report_folders[0], report_folders[1] != NULL ? report_folders[1] : "-",
                    found == Report_Summarised ? path : found == Report_Unreadable ? "only one that can't be read"
                                                                                  : "none yet");
    if (found != Report_Summarised && !last)
        return false;
    time_t t = time(NULL);
    struct tm local;
    localtime_r(&t, &local);
    char added[96];
    snprintf(added, sizeof(added), "---- added by BO3MacFix at the next start, %04d-%02d-%02d %02d:%02d:%02d:",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    if (found == Report_Summarised) {
        char exception[256], thread[256];
        snprintf(text, sizeof(text), "\n%s macOS's crash report of this run, %s\n%s", added, path, summary);
        add_to_crash_log(text);
        summary_line(summary, "exception: ", exception, sizeof(exception));
        summary_line(summary, "crashed thread", thread, sizeof(thread));
        thread_line("BO3MacFix: crash logs: macOS's report of the run before is %s; %s; %s; its summary is in %s", path,
                    exception, thread, state.kept.crash_name);
    } else if (found == Report_Unreadable) {
        snprintf(text, sizeof(text), "\n%s macOS's crash report %s of this run's time couldn't be read whole: %s", added,
                 path, summary);
        add_to_crash_log(text);
        thread_line("BO3MacFix: crash logs: macOS's report %s may be the run before's, but couldn't be read whole", path);
    } else if (state.kept.previous == Previous_Error) {
        snprintf(text, sizeof(text),
                 "\n%s macOS wrote no crash report of this run (pid %d) in %s or %s by a minute after the next start: "
                 "it ended with an error, which leaves none.\n", added, start->pid, report_folders[0],
                 report_folders[1] != NULL ? report_folders[1] : "~/Library/Logs/DiagnosticReports");
        add_to_crash_log(text);
        thread_line("BO3MacFix: crash logs: no crash report of macOS for the run before (pid %d) a minute after this "
                    "start; a run that ends with an error leaves none", start->pid);
    } else {
        snprintf(text, sizeof(text),
                 "\n%s macOS wrote no crash report of this run (pid %d) in %s or %s by a minute after the next start. A "
                 "force quit or a power cut leaves none.\n", added, start->pid, report_folders[0],
                 report_folders[1] != NULL ? report_folders[1] : "~/Library/Logs/DiagnosticReports");
        add_to_crash_log(text);
        thread_line("BO3MacFix: crash logs: no crash report of macOS for the run before (pid %d) a minute after this "
                    "start; a force quit or a power cut leaves none", start->pid);
    }
    return true;
}

// quiet: seconds without a time line before this one, when ticks were skipped (the Mac slept); 0 otherwise
static void time_line(double quiet) {
    char text[200], menu[sizeof(newest_menu)];
    int left_out;
    os_unfair_lock_lock(&lock);
    memcpy(menu, newest_menu, sizeof(menu));
    left_out = menu_limit.left_out;
    menu_limit.left_out = 0;
    os_unfair_lock_unlock(&lock);
    time_now(text, sizeof(text));
    char more[48] = "", gap[120] = "";
    if (left_out > 0)
        snprintf(more, sizeof(more), ", %d menu lines left out", left_out);
    if (quiet >= 120)
        snprintf(gap, sizeof(gap), ", nothing written for %.0f min before this (the Mac slept, or the game was stopped)",
                 quiet / 60);
    else if (quiet > 0)
        snprintf(gap, sizeof(gap), ", nothing written for %.1f s before this (the Mac slept, or the game was stopped)",
                 quiet);
    thread_line("BO3MacFix: %s%s%s%s%s", text, menu[0] != '\0' ? ", menu " : "", menu, more, gap);
}

static void sleep_until(double when) {
    for (;;) {
        double left = when - now();
        if (left <= 0)
            return;
        struct timespec wait = { (time_t)left, (long)((left - (double)(time_t)left) * 1e9) };
        nanosleep(&wait, NULL);
    }
}

// whether macOS's report of the run before is looked for: for a crash log kept of a crash or an error
static bool report_wanted(const crash_log_kept_t *kept) {
    return (kept->previous == Previous_Crashed || kept->previous == Previous_Error) && kept->crash_name[0] != '\0';
}

static void *clock_main(void *unused) {
    (void)unused;
    pthread_setname_np("BO3MacFix crash log");
    bool look = state.rotating && report_wanted(&state.kept);
    if (look)
        look = !look_for_report(false);
    // CLOCK_MONOTONIC goes on while the Mac sleeps, so after a sleep every minute missed is past at once: those are
    // skipped, and the next line says how long nothing was written
    for (long tick = 1, last_tick = 0;; tick++) {
        sleep_until(state.started + (double)tick * clock_interval);
        long due = (long)((now() - state.started) / clock_interval);
        if (due > tick)
            tick = due;
        if (look) {
            look_for_report(true);
            look = false;
        }
        time_line(tick - last_tick > 1 ? (double)(tick - last_tick) * clock_interval : 0);
        last_tick = tick;
    }
    return NULL;
}

void crash_log_started(uint64_t base_address, const char *settings_path) {
    if (!state.enabled)
        return;
    state.base_address = base_address;
    state.log_fd = fileno(stdout);
    if (state.test && !state.to_file)
        link_output();
    time_t t = time(NULL);
    struct tm local;
    localtime_r(&t, &local);
    crash_log_start_t start = { local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                                local.tm_sec, (int)(local.tm_gmtoff / 60), getpid() };
    char line[160], macos[96], power[64], settings[96];
    crash_log_format_start(&start, line, sizeof(line));
    macos_text(macos, sizeof(macos));
    power_text(power, sizeof(power));
    settings_text(settings_path, settings, sizeof(settings));
    printf("%s (KeepCrashLogs)\n", line);
    if (MACFIX_BUILD[0] != '\0')
        printf("BO3MacFix: build %s, commit %s, %s, %s, %s\n", MACFIX_VERSION, MACFIX_BUILD, macos, power, settings);
    else
        printf("BO3MacFix: build %s, built %s %s, %s, %s, %s\n", MACFIX_VERSION, __DATE__, __TIME__, macos, power,
               settings);
    if (state.rotating)
        say_previous();
    else
        printf("BO3MacFix: crash logs: this start's output doesn't go to log.txt, so none are kept\n");

    const char *home = getenv("HOME");
    snprintf(user_report_folder, sizeof(user_report_folder), "%s/Library/Logs/DiagnosticReports",
             home != NULL ? home : "");
    if (report_folders[0] == NULL) {
        report_folders[0] = "/Library/Logs/DiagnosticReports";
        report_folders[1] = home != NULL ? user_report_folder : NULL;
    }
    if (game_executables[0] == NULL)
        find_game_programs();
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_UTILITY, 0);
    if (pthread_create(&thread, &attr, clock_main, NULL) != 0)
        printf("BO3MacFix: crash logs: couldn't start the clock's thread, no time lines and no look at macOS's report\n");
    pthread_attr_destroy(&attr);
}

void crash_log_notice(void) {
    if (!state.rotating || state.kept.crash_name[0] == '\0')
        return;
    const char *slash = strrchr(state.folder, '/'), *folder = slash != NULL ? slash + 1 : state.folder;
    if (state.kept.previous == Previous_Crashed)
        notices_post_lasting("The game ended without quitting last time. Its log: %s/%s", folder,
                             state.kept.crash_name);
    else if (state.kept.previous == Previous_Error && state.kept.status != INT_MIN)
        notices_post_lasting("The game ended with an error last time (exit status %d). Its log: %s/%s",
                             state.kept.status, folder, state.kept.crash_name);
    else if (state.kept.previous == Previous_Error)
        notices_post_lasting("The game ended with an error last time. Its log: %s/%s", folder, state.kept.crash_name);
}

void crash_log_quit(int status) {
    if (!state.enabled || atomic_exchange(&state.quit_written, true))
        return;
    char text[200];
    time_now(text, sizeof(text));
    // 0 is quitting; anything else (Sys_Error's -1) an error, and the next start keeps this log as it keeps a crash's
    if (status == 0)
        printf(Crash_Log_Quit_Line ", exit status 0 (%s)\n", text);
    else
        printf(Crash_Log_Error_Line ", exit status %d, so the next start keeps this log as a crash log (%s)\n", status,
               text);
    fflush(stdout);
}

void crash_log_map(const char *map) {
    if (!state.enabled || map == NULL)
        return;
    os_unfair_lock_lock(&lock);
    snprintf(map_name, sizeof(map_name), "%s", map);
    // its UI starts with menus of its own
    newest_menu[0] = '\0';
    os_unfair_lock_unlock(&lock);
    char text[200];
    time_now(text, sizeof(text));
    printf("BO3MacFix: map %s (%s)\n", map, text);
}

#pragma mark - Lua

// the argument at index if it's a string, else NULL (without the Lua error hks_check_string raises)
static const char *string_arg(lua_State *s, int index) {
    HksObject *arg = HKS_BASE(s) + (index - 1);
    return arg < HKS_TOP(s) && (arg->t & 0xF) == HKS_TSTRING ? hks_check_string(s, index) : NULL;
}

// MenuLine(text, newest): a menu opened or closed; newest is the newest menu still open afterwards, nil for none
static int lua_menu_line(lua_State *s) {
    const char *text = string_arg(s, 1), *newest = string_arg(s, 2);
    if (text == NULL)
        return 0;
    os_unfair_lock_lock(&lock);
    snprintf(newest_menu, sizeof(newest_menu), "%s", newest != NULL ? newest : "");
    bool shown = crash_log_limit(&menu_limit, now());
    os_unfair_lock_unlock(&lock);
    if (shown) {
        char time_text[200];
        time_now(time_text, sizeof(time_text));
        printf("BO3MacFix: %.200s (%s)\n", text, time_text);
    }
    return 0;
}

void crash_log_lua_install(lua_State *s) {
    if (!state.enabled)
        return;
    // the stack can be reallocated while our Lua runs, so remember the top as an index
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    if (hks_load_source(s, (const char *)crash_log_lua, crash_log_lua_len, "=BO3MacFix crash log")) {
        hks_push_cfunction(s, lua_menu_line, "CrashLogMenuLine");
        hks_push_boolean(s, state.debug);
        hks_pcall(s, 2, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
}
