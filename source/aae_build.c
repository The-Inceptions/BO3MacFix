// Which game build All-around Enhancement expects, read from its own fastfile, see aae_build.h.
//
// AAE 3.9.5's check is a function in core_mod.ff's compiled Lua (HavokScript bytecode) whose constants include, in
// this order, "GetBuildStringField", "BuildStringField", "BUILD_STRINGFIELD_BUILD_TIME", "GetBuildIntField",
// "BuildIntField", "BUILD_INTFIELD_CHANGELIST", the number 13892626 and the string "Wed Dec 10 17:10:46 2025", next to
// "OpenURL" with its downgrade guide's link and Exec's "quit". The fastfile is a header and then runs of blocks, each
// a 16-byte header (compressed size, decompressed size, size in the file, its own offset) and zlib data, with
// signature data between the runs; the Lua is about 24 MB into the 113 MB the blocks decompress to.
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "aae_build.h"

// A compiled Lua function keeps its constants as a type byte and the value: nothing for nil, a byte for a boolean, a
// 4-byte float for a number, and for a string its size with the NUL (8 bytes) and then its bytes with the NUL.
#define Hks_Nil 0
#define Hks_Boolean 1
#define Hks_Number 3
#define Hks_String 4
#define Hks_String_Header 9

// how many constants after a field's name may come before the value it's compared with
#define Constants_After_Name 32
// changelists have 7 or 8 digits: a smaller number near the name is something else
#define Changelist_Min 100000

// reads the constant at p: its type, where its value starts and where the next constant starts. False if there's no
// constant at p that fits before len, which ends the function's constants as far as we're concerned.
static bool hks_constant(const uint8_t *data, size_t len, size_t p, int *type, size_t *value, size_t *next) {
    if (p >= len)
        return false;
    *value = p + 1;
    switch (data[p]) {
    case Hks_Nil:
        *next = p + 1;
        break;
    case Hks_Boolean:
        if (len - p < 2 || data[p + 1] > 1)
            return false;
        *next = p + 2;
        break;
    case Hks_Number:
        if (len - p < 5)
            return false;
        *next = p + 5;
        break;
    case Hks_String: {
        if (len - p < Hks_String_Header)
            return false;
        uint64_t size = 0;
        for (int i = 8; i >= 1; i--)
            size = size << 8 | data[p + i];
        if (size == 0 || size > len - p - Hks_String_Header || data[p + Hks_String_Header + size - 1] != '\0')
            return false;
        *value = p + Hks_String_Header;
        *next = *value + size;
        break;
    }
    default:
        return false;
    }
    *type = data[p];
    return true;
}

// a number constant that can be a changelist: a whole number of 6 digits or more that fits an int
static bool changelist_number(const uint8_t *value, int *out) {
    float number;
    memcpy(&number, value, sizeof(number));
    if (!(number >= Changelist_Min && number < 2147483648.0f) || number != floorf(number))
        return false;
    *out = (int)number;
    return true;
}

bool aae_build_time_valid(const char *s, size_t n) {
    const char *end = s + n;
    // the day and the month, "Wed Dec "
    for (int word = 0; word < 2; word++) {
        if (end - s < 4 || !isupper((unsigned char)s[0]) || !islower((unsigned char)s[1]) ||
            !islower((unsigned char)s[2]) || s[3] != ' ')
            return false;
        s += 4;
    }
    // the day of the month, " 3" or "3" or "10"
    if (s < end && *s == ' ')
        s++;
    int digits = 0;
    while (s < end && digits < 2 && isdigit((unsigned char)*s)) {
        s++;
        digits++;
    }
    static const char rest[] = " ##:##:## ####";
    if (digits == 0 || (size_t)(end - s) != sizeof(rest) - 1)
        return false;
    for (size_t i = 0; i < sizeof(rest) - 1; i++) {
        if (rest[i] == '#' ? !isdigit((unsigned char)s[i]) : s[i] != rest[i])
            return false;
    }
    return true;
}

static int found_mask(const aae_build_values *values) {
    return (values->changelist > 0 ? AAE_FOUND_CHANGELIST : 0) |
           (values->build_time[0] != '\0' ? AAE_FOUND_BUILD_TIME : 0);
}

// looks at the constants after each place name is a string constant, until take likes one of them
static void scan_after_name(const uint8_t *data, size_t len, const char *name, aae_build_values *out,
                            bool (*take)(const uint8_t *data, int type, size_t value, size_t next,
                                         aae_build_values *out)) {
    uint8_t needle[64];
    size_t size = strlen(name) + 1;
    needle[0] = Hks_String;
    for (int i = 0; i < 8; i++)
        needle[1 + i] = (uint8_t)(size >> (8 * i));
    memcpy(needle + Hks_String_Header, name, size);
    size_t needle_len = Hks_String_Header + size;
    for (size_t from = 0; from < len;) {
        const uint8_t *found = memmem(data + from, len - from, needle, needle_len);
        if (found == NULL)
            return;
        size_t p = (size_t)(found - data) + needle_len;
        for (int i = 0; i < Constants_After_Name; i++) {
            int type;
            size_t value, next;
            if (!hks_constant(data, len, p, &type, &value, &next))
                break;
            if (take(data, type, value, next, out))
                return;
            p = next;
        }
        from = (size_t)(found - data) + 1;
    }
}

static bool take_changelist(const uint8_t *data, int type, size_t value, size_t next, aae_build_values *out) {
    (void)next;
    return type == Hks_Number && changelist_number(data + value, &out->changelist);
}

static bool take_build_time(const uint8_t *data, int type, size_t value, size_t next, aae_build_values *out) {
    size_t n = next - value - 1; // without the NUL
    if (type != Hks_String || n >= sizeof(out->build_time) || !aae_build_time_valid((const char *)data + value, n))
        return false;
    memcpy(out->build_time, data + value, n);
    out->build_time[n] = '\0';
    return true;
}

int aae_build_scan(const uint8_t *data, size_t len, aae_build_values *out) {
    if (out->changelist <= 0)
        scan_after_name(data, len, "BUILD_INTFIELD_CHANGELIST", out, take_changelist);
    if (out->build_time[0] == '\0')
        scan_after_name(data, len, "BUILD_STRINGFIELD_BUILD_TIME", out, take_build_time);
    return found_mask(out);
}

// the fastfile header, then the blocks
#define Fastfile_Header_Size 0x248
#define Block_Header_Size 16
#define Max_Block_Size (16u << 20)
// how much of the data before a block is scanned again with it, so values just past a block's end are still found
#define Carry_Size 16384

typedef struct {
    uint32_t compressed, decompressed, size, offset;
} block_header;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// whether h, read at pos, is a block header: one that gives its own offset, with sizes that fit the file
static bool block_at(const uint8_t *h, uint64_t pos, uint64_t file_size, block_header *b) {
    b->compressed = le32(h);
    b->decompressed = le32(h + 4);
    b->size = le32(h + 8);
    b->offset = le32(h + 12);
    return b->offset == pos && b->compressed > 0 && b->compressed <= b->size && b->size - b->compressed < 64 &&
           b->size <= Max_Block_Size && b->decompressed <= Max_Block_Size &&
           pos + Block_Header_Size + b->size <= file_size;
}

// the next block header from pos on, past the data between runs of blocks, or 0 if there's none
static uint64_t next_block(int fd, uint64_t pos, uint64_t file_size, block_header *b) {
    uint8_t chunk[16384];
    pos = (pos + 3) & ~(uint64_t)3;
    while (pos + Block_Header_Size <= file_size) {
        ssize_t n = pread(fd, chunk, sizeof(chunk), (off_t)pos);
        if (n < Block_Header_Size)
            return 0;
        size_t i = 0;
        for (; i + Block_Header_Size <= (size_t)n; i += 4) {
            if (block_at(chunk + i, pos + i, file_size, b))
                return pos + i;
        }
        pos += i; // the next chunk starts with the first offset not looked at yet
    }
    return 0;
}

// decompresses a block, returns its size or -1
static long inflate_block(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, int window_bits) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, window_bits) != Z_OK)
        return -1;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)in_len;
    z.next_out = out;
    z.avail_out = (uInt)out_len;
    int result = inflate(&z, Z_FINISH);
    long produced = (long)(out_len - z.avail_out);
    inflateEnd(&z);
    return result == Z_STREAM_END ? produced : -1;
}

int aae_build_read_fastfile(const char *path, aae_build_values *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    uint8_t magic[4];
    if (fstat(fd, &st) != 0 || pread(fd, magic, sizeof(magic), 0) != sizeof(magic) || memcmp(magic, "TAff", 4) != 0) {
        close(fd);
        return -1;
    }
    uint64_t file_size = (uint64_t)st.st_size;
    uint8_t *packed = NULL, *data = NULL;
    size_t packed_size = 0, data_size = 0, carry = 0;
    uint64_t pos = Fastfile_Header_Size;
    while (found_mask(out) != AAE_FOUND_BOTH) {
        uint8_t h[Block_Header_Size];
        block_header b;
        if (pread(fd, h, sizeof(h), (off_t)pos) != sizeof(h) || !block_at(h, pos, file_size, &b)) {
            pos = next_block(fd, pos, file_size, &b);
            if (pos == 0)
                break;
        }
        if (b.compressed > packed_size) {
            uint8_t *bigger = realloc(packed, b.compressed);
            if (bigger == NULL)
                break;
            packed = bigger;
            packed_size = b.compressed;
        }
        if (Carry_Size + b.decompressed > data_size) {
            uint8_t *bigger = realloc(data, Carry_Size + b.decompressed);
            if (bigger == NULL)
                break;
            data = bigger;
            data_size = Carry_Size + b.decompressed;
        }
        if (pread(fd, packed, b.compressed, (off_t)(pos + Block_Header_Size)) != (ssize_t)b.compressed)
            break;
        // zlib, else raw deflate, else stored as it is
        long n = inflate_block(packed, b.compressed, data + carry, b.decompressed, 15);
        if (n < 0)
            n = inflate_block(packed, b.compressed, data + carry, b.decompressed, -15);
        if (n < 0) {
            n = b.compressed < b.decompressed ? b.compressed : b.decompressed;
            memcpy(data + carry, packed, (size_t)n);
        }
        size_t total = carry + (size_t)n;
        aae_build_scan(data, total, out);
        carry = total < Carry_Size ? total : Carry_Size;
        memmove(data, data + total - carry, carry);
        pos += Block_Header_Size + b.size;
    }
    free(packed);
    free(data);
    close(fd);
    return found_mask(out);
}

#define Cache_Magic "BO3MacFix aae_build.cache 1"

int aae_build_cache_load(const char *path, long long size, long long mtime, aae_build_values *out) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    aae_build_values values = { 0 };
    long long cached_size = -1, cached_mtime = -1, changelist;
    bool magic = false;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        const char *stamp = line + 10;
        if (strcmp(line, Cache_Magic) == 0) {
            magic = true;
        } else if (sscanf(line, "size %lld", &cached_size) == 1 || sscanf(line, "mtime %lld", &cached_mtime) == 1) {
        } else if (sscanf(line, "changelist %lld", &changelist) == 1) {
            values.changelist = changelist >= Changelist_Min && changelist <= INT_MAX ? (int)changelist : 0;
        } else if (strncmp(line, "buildtime ", 10) == 0 && strlen(stamp) < sizeof(values.build_time) &&
                   aae_build_time_valid(stamp, strlen(stamp))) {
            strcpy(values.build_time, stamp);
        }
    }
    fclose(fp);
    if (!magic || cached_size != size || cached_mtime != mtime)
        return -1;
    *out = values;
    return found_mask(out);
}

bool aae_build_cache_save(const char *path, long long size, long long mtime, const aae_build_values *values) {
    char temp[PATH_MAX + 32];
    if (snprintf(temp, sizeof(temp), "%s.tmp%d", path, (int)getpid()) >= (int)sizeof(temp))
        return false;
    FILE *fp = fopen(temp, "w");
    if (fp == NULL)
        return false;
    fprintf(fp, "%s\n// the game build All-around Enhancement's core_mod.ff expects, delete to read it again\n", Cache_Magic);
    fprintf(fp, "size %lld\nmtime %lld\nchangelist %d\nbuildtime %s\n", size, mtime, values->changelist,
            values->build_time);
    bool failed = ferror(fp) != 0;
    failed = fclose(fp) != 0 || failed;
    if (failed || rename(temp, path) != 0) {
        unlink(temp);
        return false;
    }
    return true;
}

// the lookup: 0 not started, 1 running, 2 done (what it found is in file_values)
static atomic_int lookup_state;
static aae_build_values file_values;
static int changelist_override;
static char build_time_override[sizeof(file_values.build_time)];
static char fastfile_path[PATH_MAX], cache_path[PATH_MAX];
static atomic_bool changelist_logged, build_time_logged, early_logged;

// what Lua is told for each value, and where that comes from
static int changelist_answer(const char **source) {
    if (changelist_override > 0) {
        *source = "AAEChangelist";
        return changelist_override;
    }
    if (atomic_load(&lookup_state) == 2 && file_values.changelist > 0) {
        *source = "core_mod.ff";
        return file_values.changelist;
    }
    *source = "built in";
    return AAE_BUILTIN_CHANGELIST;
}

static const char *build_time_answer(const char **source) {
    if (build_time_override[0] != '\0') {
        *source = "AAEBuildTime";
        return build_time_override;
    }
    if (atomic_load(&lookup_state) == 2 && file_values.build_time[0] != '\0') {
        *source = "core_mod.ff";
        return file_values.build_time;
    }
    *source = "built in";
    return AAE_BUILTIN_BUILD_TIME;
}

// the one line saying what Lua will be told and why
static void log_outcome(const char *how) {
    const char *changelist_source, *build_time_source;
    int changelist = changelist_answer(&changelist_source);
    const char *build_time = build_time_answer(&build_time_source);
    printf("AAE build check: changelist %d (%s), build time \"%s\" (%s), %s\n", changelist, changelist_source,
           build_time, build_time_source, how);
}

static void *lookup_main(void *arg) {
    (void)arg;
    pthread_setname_np("BO3MacFix AAE build");
    aae_build_values values = { 0 };
    char how[PATH_MAX + 128];
    struct stat st;
    if (stat(fastfile_path, &st) != 0) {
        snprintf(how, sizeof(how), "All-around Enhancement isn't installed (no %s)", fastfile_path);
    } else {
        int found = aae_build_cache_load(cache_path, st.st_size, st.st_mtime, &values);
        long ms = -1;
        if (found < 0) {
            struct timespec start, end;
            clock_gettime(CLOCK_MONOTONIC, &start);
            found = aae_build_read_fastfile(fastfile_path, &values);
            clock_gettime(CLOCK_MONOTONIC, &end);
            ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
            if (found >= 0 && !aae_build_cache_save(cache_path, st.st_size, st.st_mtime, &values))
                printf("AAE build check: couldn't save %s\n", cache_path);
        }
        char when[64] = "as remembered in aae_build.cache";
        if (ms >= 0)
            snprintf(when, sizeof(when), "core_mod.ff read in %ld ms", ms);
        if (found < 0)
            snprintf(how, sizeof(how), "couldn't read %s", fastfile_path);
        else if (found == AAE_FOUND_BOTH)
            snprintf(how, sizeof(how), "%s", when);
        else
            snprintf(how, sizeof(how), "%s: no %s in it, so if AAE opens its downgrade guide, set AAEChangelist and "
                     "AAEBuildTime in settings.ini", when,
                     found == 0 ? "changelist or build time" : found == AAE_FOUND_CHANGELIST ? "build time" : "changelist");
    }
    file_values = values;
    atomic_store(&lookup_state, 2);
    log_outcome(how);
    return NULL;
}

// path from the current directory, without any .. if it's there
static void absolute_path(const char *path, char *out, size_t out_size) {
    char cwd[PATH_MAX], joined[PATH_MAX * 2];
    if (path[0] != '/' && getcwd(cwd, sizeof(cwd)) != NULL)
        snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    else
        snprintf(joined, sizeof(joined), "%s", path);
    char resolved[PATH_MAX];
    snprintf(out, out_size, "%s", realpath(joined, resolved) != NULL ? resolved : joined);
}

void aae_build_start(const char *fastfile, const char *cache, int changelist, const char *build_time) {
    int idle = 0, done = 2;
    if (!atomic_compare_exchange_strong(&lookup_state, &idle, 1) &&
        !atomic_compare_exchange_strong(&lookup_state, &done, 1))
        return; // already running
    memset(&file_values, 0, sizeof(file_values));
    atomic_store(&changelist_logged, false);
    atomic_store(&build_time_logged, false);
    atomic_store(&early_logged, false);
    changelist_override = changelist > 0 ? changelist : 0;
    build_time_override[0] = '\0';
    if (build_time != NULL && build_time[0] != '\0') {
        if (strlen(build_time) < sizeof(build_time_override))
            strcpy(build_time_override, build_time);
        else
            printf("AAE build check: AAEBuildTime is too long, ignoring it\n");
    }
    absolute_path(fastfile, fastfile_path, sizeof(fastfile_path));
    absolute_path(cache, cache_path, sizeof(cache_path));
    if (changelist_override > 0 && build_time_override[0] != '\0') {
        atomic_store(&lookup_state, 2);
        log_outcome("AAEChangelist and AAEBuildTime are both set");
        return;
    }
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, lookup_main, NULL) != 0) {
        atomic_store(&lookup_state, 2);
        log_outcome("couldn't start reading core_mod.ff");
    }
    pthread_attr_destroy(&attr);
}

// logs the first answer for each value, and once if Lua asked before the lookup was done
static void log_answer(const char *what, const char *value, const char *source, atomic_bool *logged) {
    if (strcmp(source, "built in") == 0 && atomic_load(&lookup_state) == 1 && !atomic_exchange(&early_logged, true))
        printf("AAE build check: Lua asked before core_mod.ff was read, so it gets the built-in values for now\n");
    if (!atomic_exchange(logged, true))
        printf("AAE build check: Lua asked for the %s: %s (%s)\n", what, value, source);
}

int aae_build_changelist(void) {
    const char *source;
    int changelist = changelist_answer(&source);
    char text[16];
    snprintf(text, sizeof(text), "%d", changelist);
    log_answer("changelist", text, source, &changelist_logged);
    return changelist;
}

const char *aae_build_time(void) {
    const char *source;
    const char *build_time = build_time_answer(&source);
    log_answer("build time", build_time, source, &build_time_logged);
    return build_time;
}

bool aae_build_wait(int timeout_ms) {
    for (int waited = 0; atomic_load(&lookup_state) == 1; waited++) {
        if (waited >= timeout_ms)
            return false;
        usleep(1000);
    }
    return true;
}
