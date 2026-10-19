// Tests for aae_build.c: finding the build All-around Enhancement expects in made-up Lua constants (laid out like AAE
// 3.9.5's check) and in made-up fastfiles (zlib, raw deflate and stored blocks, data between runs of blocks, the
// values cut across blocks at every point), the cache, the background lookup with its overrides and fallbacks, and
// the installed AAE's core_mod.ff if there is one.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "../source/aae_build.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Newer_Time "Thu Mar  5 09:08:07 2026"

// bytes being put together
typedef struct {
    uint8_t *data;
    size_t len, size;
} bytes;

static void put(bytes *b, const void *p, size_t n) {
    if (b->len + n > b->size) {
        b->size = (b->len + n) * 2;
        b->data = realloc(b->data, b->size);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void put_byte(bytes *b, int value) {
    uint8_t byte = (uint8_t)value;
    put(b, &byte, 1);
}

static void fill(bytes *b, int value, size_t n) {
    for (size_t i = 0; i < n; i++)
        put_byte(b, value);
}

// HavokScript constants
static void k_nil(bytes *b) {
    put_byte(b, 0);
}

static void k_bool(bytes *b, int value) {
    put_byte(b, 1);
    put_byte(b, value);
}

static void k_num(bytes *b, float value) {
    put_byte(b, 3);
    put(b, &value, 4);
}

static void k_str(bytes *b, const char *s) {
    uint64_t size = strlen(s) + 1;
    put_byte(b, 4);
    put(b, &size, 8);
    put(b, s, size);
}

// the constants of AAE 3.9.5's check, in its order, and what follows them
static void aae_check(bytes *b, float changelist, const char *build_time) {
    k_str(b, "Engine");
    k_str(b, "IsBOIII");
    k_bool(b, 1);
    k_str(b, "OpenURL");
    k_str(b, "steam://openurl/https://steamcommunity.com/sharedfiles/filedetails/?id=3710608011");
    k_str(b, "Exec");
    k_str(b, "quit");
    k_str(b, "DataSources");
    k_str(b, "MPStatsSettings");
    k_str(b, "GetBuildStringField");
    k_str(b, "Enum");
    k_str(b, "BuildStringField");
    k_str(b, "BUILD_STRINGFIELD_BUILD_TIME");
    k_str(b, "GetBuildIntField");
    k_str(b, "BuildIntField");
    k_str(b, "BUILD_INTFIELD_CHANGELIST");
    k_num(b, changelist);
    k_str(b, build_time);
    put(b, "\x01\x00\x00\x00\x45\x5a\x33\xe1\x00\x00\x00\x00", 12);
}

static int scan(const bytes *b, aae_build_values *v) {
    memset(v, 0, sizeof(*v));
    return aae_build_scan(b->data, b->len, v);
}

static void test_build_time_valid(void) {
    static const char *valid[] = { "Wed Dec 10 17:10:46 2025", Newer_Time, "Thu Mar 5 09:08:07 2026" };
    static const char *invalid[] = { "", "wed Dec 10 17:10:46 2025", "Wed DEC 10 17:10:46 2025", "Wed Dec 10 17:10:46 25",
                                     "Wed Dec 10 17:10:46 2025 ", "Wed Dec 100 17:10:46 2025", "Wed Dec 10 17-10-46 2025",
                                     "Wed Dec   1 17:10:46 2025", "13892626", "BUILD_STRINGFIELD_BUILD_TIME" };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        CHECK(aae_build_time_valid(valid[i], strlen(valid[i])));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!aae_build_time_valid(invalid[i], strlen(invalid[i])));
    // only n bytes count
    CHECK(aae_build_time_valid("Wed Dec 10 17:10:46 2025xyz", 24));
    CHECK(!aae_build_time_valid("Wed Dec 10 17:10:46 2025", 23));
}

static void test_scan(void) {
    aae_build_values v;
    bytes b = { 0 };
    // AAE 3.9.5, among other functions' bytes
    fill(&b, 0xa8, 300);
    aae_check(&b, 13892626, "Wed Dec 10 17:10:46 2025");
    fill(&b, 0x39, 300);
    CHECK(scan(&b, &v) == AAE_FOUND_BOTH);
    CHECK(v.changelist == 13892626);
    CHECK(strcmp(v.build_time, "Wed Dec 10 17:10:46 2025") == 0);

    // a later version, its day of the month padded with a space
    b.len = 0;
    aae_check(&b, 14000001, Newer_Time);
    CHECK(scan(&b, &v) == AAE_FOUND_BOTH && v.changelist == 14000001 && strcmp(v.build_time, Newer_Time) == 0);

    // cut anywhere: no reading past the end, and both only once the build time is all there
    for (size_t cut = 0; cut <= b.len; cut++) {
        aae_build_values part = { 0 };
        int found = aae_build_scan(b.data, cut, &part);
        CHECK(found == (cut >= b.len - 12 ? AAE_FOUND_BOTH : cut >= b.len - 12 - 34 ? AAE_FOUND_CHANGELIST : 0));
    }

    // the changelist checked first, with other numbers and strings in between
    b.len = 0;
    k_str(&b, "BUILD_INTFIELD_CHANGELIST");
    k_num(&b, 0);
    k_num(&b, 3);
    k_num(&b, 99999);
    k_num(&b, 123456.5f);
    k_nil(&b);
    k_num(&b, 14000001);
    k_str(&b, "GetBuildStringField");
    k_str(&b, "BUILD_STRINGFIELD_BUILD_TIME");
    k_str(&b, "Wed Dec 10");
    k_str(&b, "v3.9.6");
    k_bool(&b, 0);
    k_str(&b, Newer_Time);
    CHECK(scan(&b, &v) == AAE_FOUND_BOTH && v.changelist == 14000001 && strcmp(v.build_time, Newer_Time) == 0);

    // only one of them
    b.len = 0;
    k_str(&b, "BUILD_STRINGFIELD_BUILD_TIME");
    k_str(&b, Newer_Time);
    k_str(&b, "BUILD_INTFIELD_CHANGELIST");
    k_str(&b, "quit");
    CHECK(scan(&b, &v) == AAE_FOUND_BUILD_TIME && v.changelist == 0);

    // a value without its name, or too far after it, isn't taken
    b.len = 0;
    k_num(&b, 13892626);
    k_str(&b, "Wed Dec 10 17:10:46 2025");
    k_str(&b, "BUILD_INTFIELD_CHANGELIST");
    for (int i = 0; i < 32; i++)
        k_num(&b, 1);
    k_num(&b, 13892626);
    CHECK(scan(&b, &v) == 0);

    // the name as part of a longer string isn't it
    b.len = 0;
    k_str(&b, "BUILD_INTFIELD_CHANGELIST_X");
    k_num(&b, 13892626);
    k_str(&b, "XBUILD_STRINGFIELD_BUILD_TIME");
    k_str(&b, Newer_Time);
    CHECK(scan(&b, &v) == 0);

    // something that isn't a constant ends the search: an unknown type, a string too long for the data or without its
    // NUL, a boolean that's neither
    static const uint8_t breaks[][10] = { { 7 }, { 4, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0 }, { 4, 2, 0, 0, 0, 0, 0, 0, 0 },
                                          { 1, 2 } };
    for (size_t i = 0; i < sizeof(breaks) / sizeof(breaks[0]); i++) {
        b.len = 0;
        k_str(&b, "BUILD_INTFIELD_CHANGELIST");
        put(&b, breaks[i], i == 0 ? 1 : i == 3 ? 2 : 9);
        if (i == 2)
            put(&b, "ab", 2);
        k_num(&b, 13892626);
        CHECK(scan(&b, &v) == 0);
    }

    // a second place that names the field, after one without a value
    b.len = 0;
    k_str(&b, "BUILD_INTFIELD_CHANGELIST");
    put_byte(&b, 0x45);
    k_str(&b, "BUILD_INTFIELD_CHANGELIST");
    k_num(&b, 14000001);
    CHECK(scan(&b, &v) == AAE_FOUND_CHANGELIST && v.changelist == 14000001);

    // what out already has stays
    b.len = 0;
    aae_check(&b, 14000001, Newer_Time);
    aae_build_values have = { .changelist = 15555555 };
    CHECK(aae_build_scan(b.data, b.len, &have) == AAE_FOUND_BOTH && have.changelist == 15555555 &&
          strcmp(have.build_time, Newer_Time) == 0);

    // a build time too long for out
    b.len = 0;
    k_str(&b, "BUILD_STRINGFIELD_BUILD_TIME");
    k_str(&b, "Wed Dec 10 17:10:46 2025                                        ");
    CHECK(scan(&b, &v) == 0);
    free(b.data);
}

static char dir[] = "/tmp/bo3mf_aae_build_XXXXXX";

static void write_file(const char *path, const void *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL || fwrite(data, 1, len, fp) != len) {
        printf("can't write %s\n", path);
        exit(1);
    }
    fclose(fp);
}

static void put_u32(bytes *b, uint32_t value) {
    put(b, &value, 4);
}

// a block: zlib (with a 4 KB window like AAE's), raw deflate, or stored as it is
enum { Zlib, Raw, Stored };
static void put_block(bytes *ff, const uint8_t *data, size_t len, int how) {
    size_t packed_size = compressBound((uLong)len) + 64, packed_len = len;
    uint8_t *packed = malloc(packed_size);
    if (how == Stored) {
        memcpy(packed, data, len);
    } else {
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (deflateInit2(&z, 9, Z_DEFLATED, how == Zlib ? 12 : -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            exit(1);
        z.next_in = (Bytef *)data;
        z.avail_in = (uInt)len;
        z.next_out = packed;
        z.avail_out = (uInt)packed_size;
        if (deflate(&z, Z_FINISH) != Z_STREAM_END)
            exit(1);
        packed_len = packed_size - z.avail_out;
        deflateEnd(&z);
    }
    uint32_t size = (uint32_t)(packed_len + 3) & ~3u;
    put_u32(ff, (uint32_t)packed_len);
    put_u32(ff, (uint32_t)len);
    put_u32(ff, size);
    put_u32(ff, (uint32_t)ff->len - 12);
    put(ff, packed, packed_len);
    fill(ff, 0, size - packed_len);
    free(packed);
}

// a fastfile laid out like AAE's: the header, a run of blocks ending in an empty one, other data, more blocks, and
// some data at the end. Blocks of block_size bytes of zone, mostly zlib, some raw deflate, and stored ones where the
// data starts with a byte neither can start with.
static void make_fastfile(bytes *ff, const bytes *zone, size_t block_size) {
    ff->len = 0;
    put(ff, "TAff0000\x51\x02\x00\x00\x00\x01\x00\x00", 16);
    fill(ff, 0, 0x248 - 16);
    for (size_t at = 0, i = 0; at < zone->len; at += block_size, i++) {
        size_t len = zone->len - at < block_size ? zone->len - at : block_size;
        int how = i % 3 == 1 ? Raw : i % 3 == 2 && zone->data[at] == 0xff ? Stored : Zlib;
        put_block(ff, zone->data + at, len, how);
        if (i == 2) {
            // an empty block of padding, then data between the runs of blocks
            uint8_t padding[100];
            memset(padding, 0xaf, sizeof(padding));
            put_u32(ff, sizeof(padding));
            put_u32(ff, 0);
            put_u32(ff, sizeof(padding));
            put_u32(ff, (uint32_t)ff->len - 12);
            put(ff, padding, sizeof(padding));
            for (int j = 0; j < 1000; j++)
                put_byte(ff, j * 7);
            fill(ff, 0x5a, (4 - ff->len % 4) % 4);
        }
    }
    fill(ff, 0x33, 72);
}

// zone data with AAE's check in it, after start bytes
static void make_zone(bytes *zone, size_t start, float changelist, const char *build_time) {
    zone->len = 0;
    fill(zone, 0xff, start);
    aae_check(zone, changelist, build_time);
    fill(zone, 0xff, 2000);
}

static int read_fastfile(const char *path, aae_build_values *v) {
    memset(v, 0, sizeof(*v));
    return aae_build_read_fastfile(path, v);
}

static void test_fastfile(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/core_mod.ff", dir);
    bytes zone = { 0 }, ff = { 0 };
    aae_build_values v;
    make_zone(&zone, 3000, 14000001, Newer_Time);
    // blocks of every size around the check's 570 bytes put the cut between blocks at every point of it
    for (size_t block_size = 64; block_size <= 4096; block_size += block_size < 700 ? 1 : 97) {
        make_fastfile(&ff, &zone, block_size);
        write_file(path, ff.data, ff.len);
        int found = read_fastfile(path, &v);
        if (found != AAE_FOUND_BOTH || v.changelist != 14000001 || strcmp(v.build_time, Newer_Time) != 0) {
            printf("FAIL blocks of %zu bytes: found %d, %d \"%s\"\n", block_size, found, v.changelist, v.build_time);
            failures++;
        }
    }
    // without the check
    zone.len = 0;
    fill(&zone, 0xff, 9000);
    make_fastfile(&ff, &zone, 1000);
    write_file(path, ff.data, ff.len);
    CHECK(read_fastfile(path, &v) == 0);
    // cut off anywhere: nothing until the blocks start, and both from the end of the block the check ends in on
    make_zone(&zone, 3000, 14000001, Newer_Time);
    make_fastfile(&ff, &zone, 512);
    bool both = false;
    for (size_t len = 0; len <= ff.len; len = len + 7 < ff.len || len == ff.len ? len + 7 : ff.len) {
        write_file(path, ff.data, len);
        int found = read_fastfile(path, &v);
        CHECK(found == (len < 4 ? -1 : len < 0x248 + 16 ? 0 : both ? AAE_FOUND_BOTH : found));
        CHECK(found == -1 || found == 0 || found == AAE_FOUND_CHANGELIST || found == AAE_FOUND_BOTH);
        both = both || found == AAE_FOUND_BOTH;
    }
    CHECK(both);
    // not a fastfile, or not there
    write_file(path, "TAkf0000 and so on", 18);
    CHECK(read_fastfile(path, &v) == -1);
    unlink(path);
    CHECK(read_fastfile(path, &v) == -1);
    free(zone.data);
    free(ff.data);
}

static void test_cache(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/aae_build.cache", dir);
    aae_build_values v = { .changelist = 14000001 }, got;
    strcpy(v.build_time, Newer_Time);
    CHECK(aae_build_cache_load(path, 100, 200, &got) == -1);
    CHECK(aae_build_cache_save(path, 54146432, 1760598540, &v));
    memset(&got, 0, sizeof(got));
    CHECK(aae_build_cache_load(path, 54146432, 1760598540, &got) == AAE_FOUND_BOTH);
    CHECK(got.changelist == 14000001 && strcmp(got.build_time, Newer_Time) == 0);
    // another file
    CHECK(aae_build_cache_load(path, 54146433, 1760598540, &got) == -1);
    CHECK(aae_build_cache_load(path, 54146432, 1760598541, &got) == -1);
    // a fastfile without the values is remembered too
    aae_build_values none = { 0 };
    CHECK(aae_build_cache_save(path, 100, 200, &none));
    CHECK(aae_build_cache_load(path, 100, 200, &got) == 0 && got.changelist == 0 && got.build_time[0] == '\0');
    // not ours, or values that can't be right
    const char *bad = "size 100\nmtime 200\nchangelist 14000001\n";
    write_file(path, bad, strlen(bad));
    CHECK(aae_build_cache_load(path, 100, 200, &got) == -1);
    const char *odd = "BO3MacFix aae_build.cache 1\r\nsize 100\r\nmtime 200\r\nchangelist 12\r\nbuildtime soon\r\n";
    write_file(path, odd, strlen(odd));
    CHECK(aae_build_cache_load(path, 100, 200, &got) == 0);
    const char *crlf = "BO3MacFix aae_build.cache 1\r\nsize 100\r\nmtime 200\r\nchangelist 14000001\r\nbuildtime " Newer_Time
                       "\r\n";
    write_file(path, crlf, strlen(crlf));
    CHECK(aae_build_cache_load(path, 100, 200, &got) == AAE_FOUND_BOTH && strcmp(got.build_time, Newer_Time) == 0);
    unlink(path);
}

static void check_answers(int changelist, const char *build_time) {
    CHECK(aae_build_wait(5000));
    int got_changelist = aae_build_changelist();
    const char *got_build_time = aae_build_time();
    if (got_changelist != changelist || strcmp(got_build_time, build_time) != 0) {
        printf("FAIL answers %d \"%s\", expected %d \"%s\"\n", got_changelist, got_build_time, changelist, build_time);
        failures++;
    }
}

static void test_lookup(void) {
    char ff_path[256], cache_path[256];
    snprintf(ff_path, sizeof(ff_path), "%s/core_mod.ff", dir);
    snprintf(cache_path, sizeof(cache_path), "%s/aae_build.cache", dir);
    struct stat st;
    // before anything: the built-in values
    CHECK(aae_build_changelist() == AAE_BUILTIN_CHANGELIST && strcmp(aae_build_time(), AAE_BUILTIN_BUILD_TIME) == 0);
    // no AAE: the built-in values, and nothing cached
    unlink(ff_path);
    aae_build_start(ff_path, cache_path, 0, "");
    check_answers(AAE_BUILTIN_CHANGELIST, AAE_BUILTIN_BUILD_TIME);
    CHECK(stat(cache_path, &st) != 0);
    // a newer AAE: its values, and they're cached
    bytes zone = { 0 }, ff = { 0 };
    make_zone(&zone, 50000, 14000001, Newer_Time);
    make_fastfile(&ff, &zone, 30000);
    write_file(ff_path, ff.data, ff.len);
    aae_build_start(ff_path, cache_path, 0, NULL);
    check_answers(14000001, Newer_Time);
    aae_build_values cached;
    CHECK(stat(ff_path, &st) == 0 && aae_build_cache_load(cache_path, st.st_size, st.st_mtime, &cached) == AAE_FOUND_BOTH);
    CHECK(cached.changelist == 14000001);
    // next time the cache answers, without reading the fastfile
    aae_build_values remembered = { .changelist = 15555555 };
    strcpy(remembered.build_time, "Sat Jan  2 03:04:05 2027");
    CHECK(aae_build_cache_save(cache_path, st.st_size, st.st_mtime, &remembered));
    aae_build_start(ff_path, cache_path, 0, NULL);
    check_answers(15555555, "Sat Jan  2 03:04:05 2027");
    // until the fastfile changes
    struct timeval times[2] = { { st.st_mtime + 60, 0 }, { st.st_mtime + 60, 0 } };
    CHECK(utimes(ff_path, times) == 0);
    aae_build_start(ff_path, cache_path, 0, NULL);
    check_answers(14000001, Newer_Time);
    // AAEChangelist and AAEBuildTime win, one or both
    aae_build_start(ff_path, cache_path, 13000000, NULL);
    check_answers(13000000, Newer_Time);
    aae_build_start(ff_path, cache_path, 0, "Fri Jan  1 00:00:00 2027");
    check_answers(14000001, "Fri Jan  1 00:00:00 2027");
    aae_build_start(ff_path, cache_path, 13000000, "Fri Jan  1 00:00:00 2027");
    check_answers(13000000, "Fri Jan  1 00:00:00 2027");
    // an AAE whose check we can't find: the built-in values, remembered as not found
    zone.len = 0;
    fill(&zone, 0xff, 9000);
    make_fastfile(&ff, &zone, 1000);
    write_file(ff_path, ff.data, ff.len);
    times[0].tv_sec = times[1].tv_sec = st.st_mtime + 120;
    CHECK(utimes(ff_path, times) == 0);
    aae_build_start(ff_path, cache_path, 0, NULL);
    check_answers(AAE_BUILTIN_CHANGELIST, AAE_BUILTIN_BUILD_TIME);
    CHECK(stat(ff_path, &st) == 0 && aae_build_cache_load(cache_path, st.st_size, st.st_mtime, &cached) == 0);
    // a relative path is taken from the current directory
    char cwd[1024];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL && chdir(dir) == 0);
    make_zone(&zone, 100, 14000001, Newer_Time);
    make_fastfile(&ff, &zone, 30000);
    write_file(ff_path, ff.data, ff.len);
    aae_build_start("core_mod.ff", "aae_build.cache", 0, NULL);
    CHECK(chdir(cwd) == 0);
    check_answers(14000001, Newer_Time);
    unlink(ff_path);
    unlink(cache_path);
    free(zone.data);
    free(ff.data);
}

// the installed All-around Enhancement, if there is one
static void test_installed_aae(void) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/Library/Application Support/Steam/steamapps/workshop/content/311210/2631943123/"
             "core_mod.ff", getenv("HOME") != NULL ? getenv("HOME") : "");
    if (access(path, R_OK) != 0) {
        printf("no All-around Enhancement installed, skipping the real core_mod.ff\n");
        return;
    }
    aae_build_values v;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int found = read_fastfile(path, &v);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
    CHECK(found == AAE_FOUND_BOTH);
    printf("installed AAE's core_mod.ff expects changelist %d and build time \"%s\" (read in %ld ms)\n", v.changelist,
           v.build_time, ms);
    if (found == AAE_FOUND_BOTH && (v.changelist != AAE_BUILTIN_CHANGELIST || strcmp(v.build_time, AAE_BUILTIN_BUILD_TIME) != 0))
        printf("NOTE: that's not the built-in %d \"%s\": AAE has been updated, update them in aae_build.h\n",
               AAE_BUILTIN_CHANGELIST, AAE_BUILTIN_BUILD_TIME);
}

int main(void) {
    if (mkdtemp(dir) == NULL)
        return 1;
    test_build_time_valid();
    test_scan();
    test_fastfile();
    test_cache();
    test_lookup();
    test_installed_aae();
    rmdir(dir);
    if (failures != 0) {
        printf("%d AAE build test(s) failed\n", failures);
        return 1;
    }
    puts("AAE build tests passed");
    return 0;
}
