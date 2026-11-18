// See crash_report.h. The JSON is never turned into a tree: the text is checked once as a whole (nesting bounded), then
// each value the summary wants is found by walking the text again from its object or array. A summary wants about a
// hundred values, each walk is bounded by the text's length, which is bounded by Crash_Report_Max_Size, and nothing
// here allocates, so a report of any content ends in bounded time. Every index is compared with the end before it is
// read, also where the check of the whole text already makes it impossible to be past it.
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "crash_report.h"

#define Game_Base 0x100000000ull // where wip/listing/mac.asm and offsets.h put the game
#define Game_Size (1ull << 32)   // offsets past this aren't the game's (it is 26 MB)
#define Max_Image_Index 65535    // a report lists a few dozen images
#define Max_Threads 4096         // threads looked at for "Main"
#define Max_Asi_Lines 4          // lines of "application specific information"
#define None SIZE_MAX            // no such value

typedef struct {
    const char *s;
    size_t n;
} json_t;

static size_t skip_space(const json_t *j, size_t i) {
    while (i < j->n && (j->s[i] == ' ' || j->s[i] == '\n' || j->s[i] == '\r' || j->s[i] == '\t'))
        i++;
    return i;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// the end of the string whose opening quote is at i: the index right after its closing quote, or None
static size_t skip_string(const json_t *j, size_t i) {
    if (i >= j->n || j->s[i] != '"')
        return None;
    for (i++; i < j->n; i++) {
        unsigned char c = (unsigned char)j->s[i];
        if (c == '"')
            return i + 1;
        if (c < 0x20)
            return None;
        if (c != '\\')
            continue;
        if (++i >= j->n)
            return None;
        switch (j->s[i]) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            break;
        case 'u':
            // four hex digits, all before the end
            if (j->n - i <= 4)
                return None;
            for (int k = 1; k <= 4; k++)
                if (hex_value(j->s[i + k]) < 0)
                    return None;
            i += 4;
            break;
        default:
            return None;
        }
    }
    return None;
}

static size_t skip_digits(const json_t *j, size_t i) {
    while (i < j->n && is_digit(j->s[i]))
        i++;
    return i;
}

// the end of the number at i, as JSON writes numbers, or None
static size_t skip_number(const json_t *j, size_t i) {
    if (i < j->n && j->s[i] == '-')
        i++;
    if (i >= j->n)
        return None;
    if (j->s[i] == '0')
        i++;
    else if (j->s[i] >= '1' && j->s[i] <= '9')
        i = skip_digits(j, i);
    else
        return None;
    if (i < j->n && j->s[i] == '.') {
        size_t digits = ++i;
        i = skip_digits(j, i);
        if (i == digits)
            return None;
    }
    if (i < j->n && (j->s[i] == 'e' || j->s[i] == 'E')) {
        i++;
        if (i < j->n && (j->s[i] == '+' || j->s[i] == '-'))
            i++;
        size_t digits = i;
        i = skip_digits(j, i);
        if (i == digits)
            return None;
    }
    return i;
}

static size_t skip_word(const json_t *j, size_t i, const char *word) {
    size_t length = strlen(word);
    if (i > j->n || j->n - i < length || memcmp(j->s + i, word, length) != 0)
        return None;
    return i + length;
}

// the end of the value that starts at i, None if there's no whole value there or it nests deeper than depth objects
// and arrays. Recursion is bounded by depth; each turn of the loop moves on by at least one character.
static size_t skip_value(const json_t *j, size_t i, int depth) {
    if (i >= j->n)
        return None;
    char open = j->s[i];
    if (open == '"')
        return skip_string(j, i);
    if (open == 't')
        return skip_word(j, i, "true");
    if (open == 'f')
        return skip_word(j, i, "false");
    if (open == 'n')
        return skip_word(j, i, "null");
    if (open != '{' && open != '[')
        return skip_number(j, i);
    if (depth <= 0)
        return None;
    char close = open == '{' ? '}' : ']';
    i = skip_space(j, i + 1);
    if (i < j->n && j->s[i] == close)
        return i + 1;
    for (;;) {
        if (open == '{') {
            i = skip_string(j, i);
            if (i == None)
                return None;
            i = skip_space(j, i);
            if (i >= j->n || j->s[i] != ':')
                return None;
            i = skip_space(j, i + 1);
        }
        i = skip_value(j, i, depth - 1);
        if (i == None)
            return None;
        i = skip_space(j, i);
        if (i >= j->n)
            return None;
        if (j->s[i] == close)
            return i + 1;
        if (j->s[i] != ',')
            return None;
        i = skip_space(j, i + 1);
    }
}

// the value of key in the object whose '{' is at object, or None (also when object is None or no object)
static size_t member(const json_t *j, size_t object, const char *key) {
    if (object >= j->n || j->s[object] != '{')
        return None;
    size_t key_length = strlen(key);
    size_t i = skip_space(j, object + 1);
    while (i < j->n && j->s[i] == '"') {
        size_t key_end = skip_string(j, i);
        if (key_end == None)
            return None;
        size_t value = skip_space(j, key_end);
        if (value >= j->n || j->s[value] != ':')
            return None;
        value = skip_space(j, value + 1);
        if (key_end - i == key_length + 2 && memcmp(j->s + i + 1, key, key_length) == 0)
            return value < j->n ? value : None;
        size_t end = skip_value(j, value, Crash_Report_Max_Depth);
        if (end == None)
            return None;
        i = skip_space(j, end);
        if (i >= j->n || j->s[i] != ',')
            return None;
        i = skip_space(j, i + 1);
    }
    return None;
}

// the first element of the array whose '[' is at array, None if it is empty or isn't an array
static size_t first_element(const json_t *j, size_t array) {
    if (array >= j->n || j->s[array] != '[')
        return None;
    size_t i = skip_space(j, array + 1);
    return i < j->n && j->s[i] != ']' ? i : None;
}

// the element after the one at item, None after the last
static size_t next_element(const json_t *j, size_t item) {
    size_t end = skip_value(j, item, Crash_Report_Max_Depth);
    if (end == None)
        return None;
    size_t i = skip_space(j, end);
    if (i >= j->n || j->s[i] != ',')
        return None;
    i = skip_space(j, i + 1);
    return i < j->n ? i : None;
}

// the element at index of the array at array, or None
static size_t element(const json_t *j, size_t array, size_t index) {
    size_t item = first_element(j, array);
    for (size_t k = 0; k < index && item != None; k++)
        item = next_element(j, item);
    return item;
}

// the whole number at i (no fraction, no exponent) as a sign and its size, if its size fits 64 bits
static bool read_number(const json_t *j, size_t i, bool *negative, uint64_t *size) {
    if (i >= j->n)
        return false;
    *negative = j->s[i] == '-';
    size_t p = *negative ? i + 1 : i;
    uint64_t value = 0;
    size_t digits = p;
    while (p < j->n && is_digit(j->s[p])) {
        unsigned digit = (unsigned)(j->s[p] - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
        p++;
    }
    if (p == digits || (p < j->n && (j->s[p] == '.' || j->s[p] == 'e' || j->s[p] == 'E')))
        return false;
    *size = value;
    return true;
}

static bool read_int(const json_t *j, size_t i, int64_t *out) {
    bool negative;
    uint64_t size;
    if (!read_number(j, i, &negative, &size))
        return false;
    if (!negative && size <= (uint64_t)INT64_MAX)
        *out = (int64_t)size;
    else if (negative && size <= (uint64_t)INT64_MAX)
        *out = -(int64_t)size;
    else if (negative && size == (uint64_t)INT64_MAX + 1)
        *out = INT64_MIN;
    else
        return false;
    return true;
}

static bool read_unsigned(const json_t *j, size_t i, uint64_t *out) {
    bool negative;
    uint64_t size;
    if (!read_number(j, i, &negative, &size) || negative)
        return false;
    *out = size;
    return true;
}

// the string at i into out, its escapes made plain, cut to fit; every byte that isn't printable ASCII becomes '?' (a
// line break or tab a space), except with utf8, for a path: then bytes from 0x80 stay, and a \u escape past ASCII
// becomes its UTF-8 (one of a surrogate pair a '?'). False, with out empty, if there's no string at i.
static bool read_any_text(const json_t *j, size_t i, char *out, size_t out_size, bool utf8) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    size_t end = skip_string(j, i);
    if (end == None)
        return false;
    size_t length = 0;
    // the characters are from i + 1 up to the closing quote at end - 1
    for (size_t p = i + 1; p + 1 < end; p++) {
        char c = j->s[p];
        if (c == '\\' && p + 2 < end) {
            p++;
            switch (j->s[p]) {
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                c = ' ';
                break;
            case 'u': {
                int code = 0;
                for (int k = 1; k <= 4; k++) {
                    int digit = p + (size_t)k + 1 < end ? hex_value(j->s[p + (size_t)k]) : -1;
                    code = digit < 0 || code < 0 ? -1 : code * 16 + digit;
                }
                p += 4;
                if (utf8 && code >= 0x80 && (code < 0xd800 || code > 0xdfff)) {
                    char bytes[3];
                    size_t count = 0;
                    if (code < 0x800) {
                        bytes[count++] = (char)(0xc0 | code >> 6);
                    } else {
                        bytes[count++] = (char)(0xe0 | code >> 12);
                        bytes[count++] = (char)(0x80 | (code >> 6 & 0x3f));
                    }
                    bytes[count++] = (char)(0x80 | (code & 0x3f));
                    // all of it or none of it
                    if (length + count < out_size) {
                        memcpy(out + length, bytes, count);
                        length += count;
                    }
                    continue;
                }
                c = code >= 0x20 && code < 0x7f ? (char)code : '?';
                break;
            }
            default:
                c = j->s[p];
                break;
            }
        } else if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7f || ((unsigned char)c > 0x7f && !utf8)) {
            c = '?';
        }
        if (length + 1 < out_size)
            out[length++] = c;
    }
    out[length] = '\0';
    return true;
}

static bool read_text(const json_t *j, size_t i, char *out, size_t out_size) {
    return read_any_text(j, i, out, out_size, false);
}

bool crash_report_path_matches(const char *report_path, const char *path) {
    if (report_path == NULL || path == NULL)
        return false;
    // the home folder's name: USER in the report, the user's own in the path
    static const char home[] = "/Users/USER/";
    if (strncmp(report_path, home, sizeof(home) - 1) == 0) {
        const char *slash = strncmp(path, "/Users/", 7) == 0 ? strchr(path + 7, '/') : NULL;
        if (slash == NULL || slash == path + 7)
            return false;
        report_path += sizeof(home) - 2; // at the slash after USER
        path = slash;
    }
    // "*" any text, slashes too; anything else itself. On a mismatch the last "*" takes one character more and the
    // rest is tried again, which finds a match where there is one for patterns of "*" alone.
    const char *star = NULL, *after_star = NULL;
    while (*path != '\0') {
        if (*report_path == '*') {
            star = report_path++;
            after_star = path;
        } else if (*report_path == *path) {
            report_path++;
            path++;
        } else if (star != NULL) {
            report_path = star + 1;
            path = ++after_star;
        } else {
            return false;
        }
    }
    while (*report_path == '*')
        report_path++;
    return *report_path == '\0';
}

// a summary being written: never past size, always a string
typedef struct {
    char *s;
    size_t size, length;
} out_t;

__attribute__((format(printf, 2, 3))) static void put(out_t *o, const char *format, ...) {
    if (o->size == 0 || o->length + 1 >= o->size)
        return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(o->s + o->length, o->size - o->length, format, args);
    va_end(args);
    if (written < 0) {
        o->s[o->length] = '\0';
        return;
    }
    size_t room = o->size - o->length - 1;
    o->length += (size_t)written < room ? (size_t)written : room;
}

// one frame: inside the game its full address, inside BO3MacFix.dylib its offset and symbol, elsewhere the library's
// name and the symbol (or the offset when macOS had no symbol)
static void put_frame(out_t *o, const json_t *j, size_t frame, size_t images, int number) {
    int64_t index = -1, location = 0;
    uint64_t offset = 0;
    bool has_index = read_int(j, member(j, frame, "imageIndex"), &index);
    bool has_offset = read_unsigned(j, member(j, frame, "imageOffset"), &offset);
    bool has_location = read_int(j, member(j, frame, "symbolLocation"), &location);
    char name[96], symbol[192];
    if (!has_index || index < 0 || index > Max_Image_Index ||
        !read_text(j, member(j, element(j, images, (size_t)index), "name"), name, sizeof(name)) || name[0] == '\0')
        snprintf(name, sizeof(name), "?");
    bool has_symbol = read_text(j, member(j, frame, "symbol"), symbol, sizeof(symbol)) && symbol[0] != '\0';
    put(o, "  %2d  ", number);
    if (strncmp(name, "CoDBlkOps3_Exe", 14) == 0 && has_offset && offset < Game_Size)
        put(o, "game 0x%" PRIx64 "\n", (uint64_t)(Game_Base + offset));
    else if (strcmp(name, "BO3MacFix.dylib") == 0 && has_offset)
        put(o, "BO3MacFix+0x%" PRIx64 "%s%s\n", offset, has_symbol ? " " : "", has_symbol ? symbol : "");
    else if (has_symbol)
        put(o, "%s %s+%" PRId64 "\n", name, symbol, has_location ? location : 0);
    else if (has_offset)
        put(o, "%s +0x%" PRIx64 "\n", name, offset);
    else
        put(o, "%s\n", name);
}

static void put_frames(out_t *o, const json_t *j, size_t thread, size_t images, int count) {
    size_t frame = first_element(j, member(j, thread, "frames"));
    if (frame == None)
        put(o, "  (no frames)\n");
    for (int number = 0; number < count && frame != None; number++) {
        put_frame(o, j, frame, images, number);
        frame = next_element(j, frame);
    }
}

// "label: text\n" when key holds a string in object
static void put_text(out_t *o, const json_t *j, size_t object, const char *key, const char *label) {
    char text[256];
    if (read_text(j, member(j, object, key), text, sizeof(text)) && text[0] != '\0')
        put(o, "%s: %s\n", label, text);
}

// the exception: its type, signal and subtype ("EXC_BAD_ACCESS, SIGSEGV, KERN_INVALID_ADDRESS at 0x...")
static void put_exception(out_t *o, const json_t *j, size_t body) {
    size_t exception = member(j, body, "exception");
    const char *keys[] = { "type", "signal", "subtype" };
    put(o, "exception:");
    bool any = false;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        char text[192];
        if (read_text(j, member(j, exception, keys[k]), text, sizeof(text)) && text[0] != '\0') {
            put(o, "%s %s", any ? "," : "", text);
            any = true;
        }
    }
    put(o, "%s\n", any ? "" : " (none given)");
}

// macOS's "application specific information": an object of libraries, each with an array of lines
static void put_asi(out_t *o, const json_t *j, size_t body) {
    size_t asi = member(j, body, "asi");
    if (asi >= j->n || j->s[asi] != '{')
        return;
    int lines = 0;
    size_t i = skip_space(j, asi + 1);
    while (i < j->n && j->s[i] == '"' && lines < Max_Asi_Lines) {
        char library[96];
        size_t key_end = skip_string(j, i);
        if (key_end == None || !read_text(j, i, library, sizeof(library)))
            return;
        size_t value = skip_space(j, key_end);
        if (value >= j->n || j->s[value] != ':')
            return;
        value = skip_space(j, value + 1);
        for (size_t line = first_element(j, value); line != None && lines < Max_Asi_Lines;
             line = next_element(j, line)) {
            char text[256];
            if (read_text(j, line, text, sizeof(text))) {
                put(o, "asi: %s: %s\n", library, text);
                lines++;
            }
        }
        size_t end = skip_value(j, value, Crash_Report_Max_Depth);
        if (end == None)
            return;
        i = skip_space(j, end);
        if (i >= j->n || j->s[i] != ',')
            return;
        i = skip_space(j, i + 1);
    }
}

crash_report_result_t crash_report_summary(const char *text, size_t length, int pid, const char *const *executables,
                                           char *out, size_t out_size) {
    out_t o = { out, out_size, 0 };
    if (out_size > 0)
        out[0] = '\0';
    if (text == NULL || length == 0) {
        put(&o, "the report is empty\n");
        return Crash_Report_Unreadable;
    }
    if (length > Crash_Report_Max_Size) {
        put(&o, "too big for a crash report (%zu bytes)\n", length);
        return Crash_Report_Unreadable;
    }
    // a one-line JSON header, then the body
    const char *newline = memchr(text, '\n', length);
    if (newline == NULL) {
        put(&o, "not a crash report: no header line\n");
        return Crash_Report_Unreadable;
    }
    json_t header = { text, (size_t)(newline - text) };
    size_t header_start = skip_space(&header, 0);
    size_t header_end = header_start < header.n && text[header_start] == '{' ? skip_value(&header, header_start, 2)
                                                                            : None;
    if (header_end == None || skip_space(&header, header_end) != header.n) {
        put(&o, "not a crash report: its first line isn't one JSON object\n");
        return Crash_Report_Unreadable;
    }
    json_t whole = { newline + 1, length - (size_t)(newline + 1 - text) };
    const json_t *j = &whole;
    size_t body = skip_space(j, 0);
    size_t body_end = body < j->n && j->s[body] == '{' ? skip_value(j, body, Crash_Report_Max_Depth) : None;
    if (body_end == None || skip_space(j, body_end) != j->n) {
        put(&o, "cut off or damaged: its body isn't one whole JSON object\n");
        return Crash_Report_Unreadable;
    }
    size_t threads = member(j, body, "threads");
    size_t exception = member(j, body, "exception");
    if (threads >= j->n || j->s[threads] != '[' || exception >= j->n || j->s[exception] != '{') {
        put(&o, "not a crash report of the kind this reads: no exception or threads\n");
        return Crash_Report_Unreadable;
    }
    int64_t report_pid = 0;
    bool has_pid = read_int(j, member(j, body, "pid"), &report_pid);
    if (pid > 0 && (!has_pid || report_pid != pid)) {
        put(&o, "the report of another process (pid %" PRId64 ")\n", has_pid ? report_pid : (int64_t)0);
        return Crash_Report_Other_Process;
    }
    if (executables != NULL) {
        char process_path[1024];
        bool named = read_any_text(j, member(j, body, "procPath"), process_path, sizeof(process_path), true);
        bool ours = false;
        for (size_t i = 0; named && executables[i] != NULL && !ours; i++)
            ours = crash_report_path_matches(process_path, executables[i]);
        if (!ours) {
            put(&o, "the report of a program elsewhere (%s)\n", named ? process_path : "no procPath");
            return Crash_Report_Other_Process;
        }
    }

    char name[128], launched[64], captured[64];
    if (!read_text(j, member(j, body, "procName"), name, sizeof(name)) || name[0] == '\0')
        snprintf(name, sizeof(name), "?");
    if (!read_text(j, member(j, body, "procLaunch"), launched, sizeof(launched)) || launched[0] == '\0')
        snprintf(launched, sizeof(launched), "?");
    if (!read_text(j, member(j, body, "captureTime"), captured, sizeof(captured)) || captured[0] == '\0')
        snprintf(captured, sizeof(captured), "?");
    put(&o, "process: %s, pid %" PRId64 ", started %s, crashed %s\n", name, has_pid ? report_pid : (int64_t)0, launched,
        captured);
    put_exception(&o, j, body);
    put_text(&o, j, member(j, body, "termination"), "indicator", "termination");
    put_asi(&o, j, body);

    size_t images = member(j, body, "usedImages");
    // the crashed thread: macOS names it in faultingThread, and marks it "triggered"
    int64_t crashed = -1;
    size_t thread = None;
    if (read_int(j, member(j, body, "faultingThread"), &crashed) && crashed >= 0 && crashed < Max_Threads)
        thread = element(j, threads, (size_t)crashed);
    if (thread == None) {
        crashed = -1;
        int64_t index = 0;
        for (size_t item = first_element(j, threads); item != None && index < Max_Threads;
             item = next_element(j, item), index++) {
            size_t triggered = member(j, item, "triggered");
            if (skip_word(j, triggered, "true") != None) {
                thread = item;
                crashed = index;
                break;
            }
        }
    }
    if (thread == None) {
        put(&o, "crashed thread: not in the report\n");
    } else {
        char thread_name[96];
        bool named = read_text(j, member(j, thread, "name"), thread_name, sizeof(thread_name)) && thread_name[0] != '\0';
        uint64_t rip = 0;
        size_t rip_value = member(j, member(j, member(j, thread, "threadState"), "rip"), "value");
        if (read_unsigned(j, rip_value, &rip))
            put(&o, "crashed thread %" PRId64 "%s%s%s, rip 0x%" PRIx64 "\n", crashed, named ? " \"" : "",
                named ? thread_name : "", named ? "\"" : "", rip);
        else
            put(&o, "crashed thread %" PRId64 "%s%s%s, no rip in the report\n", crashed, named ? " \"" : "",
                named ? thread_name : "", named ? "\"" : "");
        put_frames(&o, j, thread, images, Crash_Report_Frames);
    }
    // the game's main thread ("Main", the one that runs the game's frame; "main" is the process's), unless it crashed
    int64_t index = 0;
    for (size_t item = first_element(j, threads); item != None && index < Max_Threads;
         item = next_element(j, item), index++) {
        char thread_name[16];
        if (index == crashed || !read_text(j, member(j, item, "name"), thread_name, sizeof(thread_name)) ||
            strcmp(thread_name, "Main") != 0)
            continue;
        put(&o, "the game's thread %" PRId64 " \"Main\":\n", index);
        put_frames(&o, j, item, images, Crash_Report_Main_Frames);
        break;
    }
    return Crash_Report_Summarised;
}

char *crash_report_read(const char *path, size_t *length, const char **why) {
    *length = 0;
    // not following a link, and not waiting for a writer if it is a pipe
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        *why = errno == EACCES || errno == EPERM ? "not allowed to read it" : "can't open it";
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *why = "not a regular file";
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > Crash_Report_Max_Size) {
        close(fd);
        *why = st.st_size <= 0 ? "empty" : "too big for a crash report";
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *buffer = malloc(size + 1);
    if (buffer == NULL) {
        close(fd);
        *why = "no memory to read it";
        return NULL;
    }
    // at most the size it had: a file still being written is read up to there
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, buffer + got, size - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    if (got == 0) {
        free(buffer);
        *why = "can't read it";
        return NULL;
    }
    buffer[got] = '\0';
    *length = got;
    *why = NULL;
    return buffer;
}
