// StartWithMod's rules and files (start_mod.h), shared by the library and the launcher.
#include "start_mod.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "launch_args.h"

// how much of lastmod.txt or the marker is read: more than a name and a line end
#define File_Read_Max 64
// the most of a workshop.json read for its title (All-around Enhancement's whole file is 2 KB)
#define Json_Read_Max (256 * 1024)
// a title as it comes, before it is cleaned and cut
#define Raw_Title_Size 256
// a title cut at a word keeps at least this much of it, else it is cut in the word
#define Title_Min_Cut 8

bool start_mod_is_stand_in(const char *name) {
    return name != NULL && strcasecmp(name, Start_Mod_Stand_In) == 0;
}

bool start_mod_name_ok(const char *name) {
    if (name == NULL || name[0] == '\0' || name[0] == '.' || name[0] == '-')
        return false;
    size_t length = 0;
    for (const char *p = name; *p != '\0'; p++) {
        char c = *p;
        bool fits = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '-' || c == '.';
        if (!fits || ++length > Start_Mod_Name_Size - 1)
            return false;
    }
    return strstr(name, "..") == NULL && !start_mod_is_stand_in(name);
}

start_mod_loaded_t start_mod_loaded(const char raw[Start_Mod_Name_Size], char name[Start_Mod_Name_Size]) {
    // the game ends its name with a 0 inside the 0x20 bytes (it refuses longer ones); without one there is no name
    size_t length = strnlen(raw, Start_Mod_Name_Size);
    bool ended = length < Start_Mod_Name_Size;
    if (!ended)
        length = Start_Mod_Name_Size - 1;
    memcpy(name, raw, length);
    name[length] = '\0';
    start_mod_loaded_t loaded = !ended                         ? Start_Mod_Loaded_Unfit
                                : name[0] == '\0'              ? Start_Mod_Loaded_None
                                : start_mod_is_stand_in(name) ? Start_Mod_Loaded_Stand_In
                                : start_mod_name_ok(name)     ? Start_Mod_Loaded_Mod
                                                              : Start_Mod_Loaded_Unfit;
    // for the log: what isn't printable as '?'
    if (loaded == Start_Mod_Loaded_Unfit)
        for (char *p = name; *p != '\0'; p++)
            if (*p < 0x20 || *p > 0x7e)
                *p = '?';
    return loaded;
}

start_mod_file_t start_mod_parse(const char *text, size_t length, char name[Start_Mod_Name_Size]) {
    name[0] = '\0';
    // one line end, as start_mod_write writes it, or as an editor saves it
    if (length > 0 && text[length - 1] == '\n') {
        length--;
        if (length > 0 && text[length - 1] == '\r')
            length--;
    }
    if (length == 0)
        return Start_Mod_File_Absent;
    if (length > Start_Mod_Name_Size - 1)
        return Start_Mod_File_Damaged;
    char copy[Start_Mod_Name_Size];
    memcpy(copy, text, length);
    copy[length] = '\0';
    // a 0 inside, or anything a name may not hold
    if (strlen(copy) != length || !start_mod_name_ok(copy))
        return Start_Mod_File_Damaged;
    memcpy(name, copy, length + 1);
    return Start_Mod_File_Named;
}

// reads at most size bytes of the file at path into text; false if it can't be read
static bool read_head(const char *path, char *text, size_t size, size_t *length) {
    *length = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    bool ok = true;
    while (*length < size) {
        ssize_t got = read(fd, text + *length, size - *length);
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        *length += (size_t)got;
    }
    close(fd);
    return ok;
}

start_mod_file_t start_mod_read(const char *path, char name[Start_Mod_Name_Size]) {
    name[0] = '\0';
    struct stat info;
    if (lstat(path, &info) != 0)
        return errno == ENOENT ? Start_Mod_File_Absent : Start_Mod_File_Damaged;
    // a folder or a link where the file should be is none of ours
    if (!S_ISREG(info.st_mode))
        return Start_Mod_File_Damaged;
    // the start of the file is enough: a name and its line end take at most 33 bytes, and the parser refuses more
    char text[File_Read_Max];
    size_t length;
    if (!read_head(path, text, sizeof(text), &length))
        return Start_Mod_File_Damaged;
    return start_mod_parse(text, length, name);
}

static bool write_all(int fd, const char *text, size_t length) {
    while (length > 0) {
        ssize_t done = write(fd, text, length);
        if (done < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        text += done;
        length -= (size_t)done;
    }
    return true;
}

bool start_mod_write(const char *path, const char *name) {
    if (name == NULL || name[0] == '\0')
        return unlink(path) == 0 || errno == ENOENT;
    if (!start_mod_name_ok(name))
        return false;
    char temp[PATH_MAX + 32], line[Start_Mod_Name_Size + 1];
    int line_length = snprintf(line, sizeof(line), "%s\n", name);
    if (snprintf(temp, sizeof(temp), "%s.tmp%d", path, (int)getpid()) >= (int)sizeof(temp) || line_length <= 0 ||
        line_length >= (int)sizeof(line))
        return false;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    // on the disk before it takes the old one's place, so that a power cut leaves the old file or the new one
    bool ok = write_all(fd, line, (size_t)line_length) && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(temp, path) != 0) {
        unlink(temp);
        return false;
    }
    return true;
}

// out = folder/file; false if it doesn't fit
static bool join(char *out, size_t size, const char *folder, const char *file) {
    int length = snprintf(out, size, "%s/%s", folder, file);
    return length > 0 && (size_t)length < size;
}

static bool all_digits(const char *name) {
    if (name[0] == '\0')
        return false;
    for (const char *p = name; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

// a zone file the game could open: a regular file with something in it
static bool zone_file_there(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

static bool folder_there(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

start_mod_files_t start_mod_files(const start_mod_paths_t *paths, const char *name) {
    char path[PATH_MAX];
    int length;
    if (!start_mod_name_ok(name))
        return Start_Mod_Files_Missing;
    // a workshop item: its folder is named by its id. Steam downloads an update next to it and then moves the files
    // over the old ones, so while it downloads the files may change under the game.
    if (all_digits(name) && paths->workshop != NULL) {
        length = snprintf(path, sizeof(path), "%s/downloads/" Start_Mod_App_Id "/%s", paths->workshop, name);
        if (length > 0 && (size_t)length < sizeof(path) && folder_there(path))
            return Start_Mod_Files_Updating;
        length = snprintf(path, sizeof(path), "%s/content/" Start_Mod_App_Id "/%s/core_mod.ff", paths->workshop, name);
        if (length > 0 && (size_t)length < sizeof(path) && zone_file_there(path))
            return Start_Mod_Files_There;
    }
    // a mod in the game's own folder, where the game looks for one it has no entry for (mods/<name>/zone)
    if (paths->game != NULL) {
        length = snprintf(path, sizeof(path), "%s/mods/%s/zone/core_mod.ff", paths->game, name);
        if (length > 0 && (size_t)length < sizeof(path) && zone_file_there(path))
            return Start_Mod_Files_There;
    }
    return Start_Mod_Files_Missing;
}

start_mod_plan_t start_mod_launch(const start_mod_paths_t *paths, bool enabled, int argc, char *const argv[],
                                  bool write, char mod[Start_Mod_Name_Size]) {
    mod[0] = '\0';
    if (!enabled)
        return Start_Mod_Off;
    char memory[PATH_MAX], marker[PATH_MAX];
    if (paths->folder == NULL || !join(memory, sizeof(memory), paths->folder, Start_Mod_Memory_File) ||
        !join(marker, sizeof(marker), paths->folder, Start_Mod_Marker_File))
        return Start_Mod_Unsafe;
    // The marker of a start given the mod is still there: that start never got as far as the menus, where the game
    // removes it (the mod stopped the game while it loaded, its files are damaged, or the Mac went off). This start
    // is the stock game, and the mod is forgotten, so that a mod that stops the game at its start can't do it at
    // every start. A marker counts whatever it holds.
    struct stat info;
    if (lstat(marker, &info) == 0) {
        if (start_mod_read(marker, mod) != Start_Mod_File_Named)
            mod[0] = '\0';
        if (write) {
            start_mod_write(memory, "");
            start_mod_write(marker, "");
        }
        return Start_Mod_Failed;
    }
    switch (start_mod_read(memory, mod)) {
    case Start_Mod_File_Absent:
        return Start_Mod_Nothing;
    case Start_Mod_File_Damaged:
        mod[0] = '\0';
        return Start_Mod_Damaged;
    case Start_Mod_File_Named:
        break;
    }
    // the player's own command line comes first: a mod of its own, or a map, a server or a lobby to join
    if (launch_args_sets_fs_game(argc, argv) || launch_args_start_game(argc, argv))
        return Start_Mod_Busy;
    switch (start_mod_files(paths, mod)) {
    case Start_Mod_Files_Missing:
        return Start_Mod_Missing;
    case Start_Mod_Files_Updating:
        return Start_Mod_Updating;
    case Start_Mod_Files_There:
        break;
    }
    if (write && !start_mod_write(marker, mod))
        return Start_Mod_Unsafe;
    return Start_Mod_Given;
}

bool start_mod_launch_abandoned(const start_mod_paths_t *paths) {
    char marker[PATH_MAX];
    return paths->folder != NULL && join(marker, sizeof(marker), paths->folder, Start_Mod_Marker_File) &&
           start_mod_write(marker, "");
}

static const struct {
    start_mod_plan_t plan;
    const char *word, *text;
} plans[] = {
    { Start_Mod_Off, "off", "StartWithMod is off" },
    { Start_Mod_Nothing, "nothing", "no mod was loaded at the last quit" },
    { Start_Mod_Damaged, "damaged", "lastmod.txt holds no mod's name, so it is ignored" },
    { Start_Mod_Failed, "failed", "the last start with it didn't get as far as the menus, so this one is without it" },
    { Start_Mod_Busy, "busy", "the command line has an fs_game of its own or starts a map" },
    { Start_Mod_Missing, "missing", "its files aren't there" },
    { Start_Mod_Updating, "updating", "Steam is downloading it" },
    { Start_Mod_Unsafe, "unsafe", "lastmod.pending couldn't be written, so no start with it" },
    { Start_Mod_Given, "given", "started with it (+set fs_game)" },
};
#define Plan_Count (sizeof(plans) / sizeof(plans[0]))

void start_mod_note(start_mod_plan_t plan, const char *mod, char *out, size_t size) {
    if (size == 0)
        return;
    out[0] = '\0';
    for (size_t i = 0; i < Plan_Count; i++)
        if (plans[i].plan == plan)
            snprintf(out, size, "%s %s", plans[i].word, start_mod_name_ok(mod) ? mod : "");
}

bool start_mod_parse_note(const char *note, start_mod_plan_t *plan, char mod[Start_Mod_Name_Size]) {
    mod[0] = '\0';
    if (note == NULL)
        return false;
    const char *space = strchr(note, ' ');
    if (space == NULL)
        return false;
    for (size_t i = 0; i < Plan_Count; i++) {
        size_t length = strlen(plans[i].word);
        if ((size_t)(space - note) != length || strncmp(note, plans[i].word, length) != 0)
            continue;
        const char *name = space + 1;
        // a name that may be remembered, or none at all (a marker that named none)
        if (name[0] != '\0' && !start_mod_name_ok(name))
            return false;
        // the launcher only starts the game with a mod it names
        if (plans[i].plan == Start_Mod_Given && name[0] == '\0')
            return false;
        snprintf(mod, Start_Mod_Name_Size, "%s", name);
        *plan = plans[i].plan;
        return true;
    }
    return false;
}

const char *start_mod_plan_text(start_mod_plan_t plan) {
    for (size_t i = 0; i < Plan_Count; i++)
        if (plans[i].plan == plan)
            return plans[i].text;
    return "?";
}

void start_mod_menu_ready(const start_mod_paths_t *paths, const char raw_loaded[Start_Mod_Name_Size],
                          start_mod_look_t *look) {
    memset(look, 0, sizeof(*look));
    look->loaded = start_mod_loaded(raw_loaded, look->loaded_name);
    char memory[PATH_MAX], marker[PATH_MAX];
    if (paths->folder == NULL || !join(memory, sizeof(memory), paths->folder, Start_Mod_Memory_File) ||
        !join(marker, sizeof(marker), paths->folder, Start_Mod_Marker_File)) {
        look->before = Start_Mod_File_Damaged;
        look->changed = true;
        return;
    }
    // this start got as far as the menus: the next one may be given the mod again
    struct stat info;
    look->marker_there = lstat(marker, &info) == 0;
    if (look->marker_there)
        look->marker_removed = start_mod_write(marker, "");
    // the mod to remember: the one loaded, unless it is the stand-in or can't go on a command line
    const char *wanted = look->loaded == Start_Mod_Loaded_Mod ? look->loaded_name : "";
    look->before = start_mod_read(memory, look->before_name);
    bool same = wanted[0] == '\0' ? look->before == Start_Mod_File_Absent
                                  : look->before == Start_Mod_File_Named && strcmp(look->before_name, wanted) == 0;
    if (!same) {
        look->changed = true;
        look->written = start_mod_write(memory, wanted);
    }
}

// skips JSON's white space
static const char *json_space(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// adds a character to out as UTF-8, if all of it fits (out may be NULL: the string is only skipped)
static void put_code(char *out, size_t size, size_t *used, uint32_t code) {
    char bytes[4];
    size_t count;
    if (code < 0x80) {
        bytes[0] = (char)code;
        count = 1;
    } else if (code < 0x800) {
        bytes[0] = (char)(0xc0 | (code >> 6));
        bytes[1] = (char)(0x80 | (code & 0x3f));
        count = 2;
    } else {
        bytes[0] = (char)(0xe0 | (code >> 12));
        bytes[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (code & 0x3f));
        count = 3;
    }
    if (out == NULL || *used + count >= size)
        return;
    memcpy(out + *used, bytes, count);
    *used += count;
}

// The JSON string that starts at p (its opening quote) into out, as UTF-8 and at most size - 1 bytes (the rest is
// dropped, the string still read to its end). Returns where it ends, after its closing quote, or NULL if it isn't one.
static const char *json_string(const char *p, const char *end, char *out, size_t size) {
    size_t used = 0;
    if (out != NULL && size > 0)
        out[0] = '\0';
    if (p >= end || *p != '"')
        return NULL;
    for (p++; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            if (out != NULL && size > 0)
                out[used] = '\0';
            return p + 1;
        }
        // JSON allows no control characters in a string as they are
        if (c < 0x20)
            return NULL;
        if (c != '\\') {
            if (out != NULL && used + 1 < size)
                out[used++] = (char)c;
            continue;
        }
        if (++p >= end)
            return NULL;
        uint32_t code;
        switch (*p) {
        case '"': code = '"'; break;
        case '\\': code = '\\'; break;
        case '/': code = '/'; break;
        case 'b': code = '\b'; break;
        case 'f': code = '\f'; break;
        case 'n': code = '\n'; break;
        case 'r': code = '\r'; break;
        case 't': code = '\t'; break;
        case 'u':
            if (end - p < 5)
                return NULL;
            code = 0;
            for (int i = 1; i <= 4; i++) {
                int digit = hex_digit(p[i]);
                if (digit < 0)
                    return NULL;
                code = code * 16 + (uint32_t)digit;
            }
            p += 4;
            // half of a pair (a character beyond the first 65536) or a 0: a stand-in, never a cut in the text
            if (code == 0 || (code >= 0xd800 && code <= 0xdfff))
                code = '?';
            break;
        default:
            return NULL;
        }
        put_code(out, size, &used, code);
    }
    return NULL;
}

// skips the JSON value at p, up to the ',' or '}' that ends it at this level; NULL if it doesn't end
static const char *json_skip(const char *p, const char *end) {
    int depth = 0;
    while (p < end) {
        char c = *p;
        if (c == '"') {
            p = json_string(p, end, NULL, 0);
            if (p == NULL)
                return NULL;
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (depth == 0)
                return p;
            depth--;
        } else if (c == ',' && depth == 0) {
            return p;
        }
        p++;
    }
    return NULL;
}

// how many bytes the UTF-8 character starting with these bytes has, 0 if they aren't one
static size_t utf8_length(const unsigned char *p, size_t left) {
    size_t count = p[0] < 0x80               ? 1
                   : (p[0] & 0xe0) == 0xc0 ? 2
                   : (p[0] & 0xf0) == 0xe0 ? 3
                   : (p[0] & 0xf8) == 0xf0 ? 4
                                           : 0;
    if (count == 0 || count > left || (count == 2 && p[0] < 0xc2))
        return 0;
    for (size_t i = 1; i < count; i++)
        if ((p[i] & 0xc0) != 0x80)
            return 0;
    return count;
}

// a title as a notice shows it: the game's colour codes (^ and a letter or digit) out, control characters and runs
// of spaces as one space, bytes that aren't UTF-8 as '?', cut to Start_Mod_Title_Max bytes at a word
static bool clean_title(const char *raw, char *out, size_t size) {
    char clean[Raw_Title_Size];
    size_t used = 0, length = strlen(raw);
    const unsigned char *p = (const unsigned char *)raw, *end = p + length;
    while (p < end && used + 4 < sizeof(clean)) {
        if (p[0] == '^' && p + 1 < end && p[1] < 0x80 &&
            ((p[1] >= '0' && p[1] <= '9') || (p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z'))) {
            p += 2;
            continue;
        }
        if (p[0] < 0x20 || p[0] == 0x7f || p[0] == ' ') {
            if (used > 0 && clean[used - 1] != ' ')
                clean[used++] = ' ';
            p++;
            continue;
        }
        size_t count = utf8_length(p, (size_t)(end - p));
        if (count == 0) {
            clean[used++] = '?';
            p++;
            continue;
        }
        memcpy(clean + used, p, count);
        used += count;
        p += count;
    }
    while (used > 0 && clean[used - 1] == ' ')
        used--;
    clean[used] = '\0';
    if (used == 0 || size == 0)
        return false;
    // at most Start_Mod_Title_Max bytes (and what out holds): at the last space that keeps a good part of it, else at
    // the start of a character
    size_t limit = size - 1 < Start_Mod_Title_Max ? size - 1 : Start_Mod_Title_Max;
    if (used > limit) {
        size_t cut = limit;
        while (cut > 0 && ((unsigned char)clean[cut] & 0xc0) == 0x80)
            cut--;
        for (size_t space = limit; space >= Title_Min_Cut; space--) {
            if (clean[space] == ' ') {
                cut = space;
                break;
            }
        }
        used = cut;
        while (used > 0 && clean[used - 1] == ' ')
            used--;
    }
    memcpy(out, clean, used);
    out[used] = '\0';
    return used > 0;
}

bool start_mod_title_from_json(const char *json, size_t length, char *out, size_t size) {
    const char *p = json, *end = json + length;
    if (length >= 3 && memcmp(p, "\xef\xbb\xbf", 3) == 0)
        p += 3;
    p = json_space(p, end);
    if (p >= end || *p != '{')
        return false;
    p++;
    for (;;) {
        p = json_space(p, end);
        if (p >= end || *p == '}')
            return false;
        char key[16];
        p = json_string(p, end, key, sizeof(key));
        if (p == NULL)
            return false;
        p = json_space(p, end);
        if (p >= end || *p != ':')
            return false;
        p = json_space(p + 1, end);
        if (strcmp(key, "Title") == 0 && p < end && *p == '"') {
            char raw[Raw_Title_Size];
            return json_string(p, end, raw, sizeof(raw)) != NULL && clean_title(raw, out, size);
        }
        p = json_skip(p, end);
        if (p == NULL)
            return false;
        if (*p == ',')
            p++;
    }
}

void start_mod_title(const start_mod_paths_t *paths, const char *name, char *out, size_t size) {
    if (size == 0)
        return;
    if (!start_mod_name_ok(name)) {
        snprintf(out, size, "the mod");
        return;
    }
    char path[PATH_MAX];
    int length = paths->workshop != NULL && all_digits(name)
                     ? snprintf(path, sizeof(path), "%s/content/" Start_Mod_App_Id "/%s/workshop.json",
                                paths->workshop, name)
                     : -1;
    if (length > 0 && (size_t)length < sizeof(path)) {
        static char json[Json_Read_Max];
        size_t read_length;
        if (read_head(path, json, sizeof(json), &read_length) &&
            start_mod_title_from_json(json, read_length, out, size))
            return;
    }
    snprintf(out, size, "the mod %s", name);
}

bool start_mod_notice_text(start_mod_plan_t plan, const char *title, char *out, size_t size) {
    switch (plan) {
    case Start_Mod_Failed:
        snprintf(out, size, "The last start with %s didn't reach the menus. This one is without it: load it from MODS.",
                 title);
        return true;
    case Start_Mod_Missing:
        snprintf(out, size, "The game started without %s: it was loaded at the last quit, but its files are gone.",
                 title);
        return true;
    case Start_Mod_Updating:
        snprintf(out, size,
                 "Steam is downloading %s, so the game started without it. Load it from MODS once it's done.",
                 title);
        return true;
    default:
        if (size > 0)
            out[0] = '\0';
        return false;
    }
}
