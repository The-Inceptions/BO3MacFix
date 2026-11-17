// aae_build.c under the fuzzer (make fuzz): the decompressed data of a mod's fastfile, scanned for the build values
// in its compiled Lua, the build time's text, and aae_build.cache (a file, written to the fuzzer's out folder). The
// fastfile walker itself (aae_build_read_fastfile) reads a file too: the input is written there as one, with a
// fastfile's magic, and walked.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../source/aae_build.c"

static char fuzz_cache_path[256], fuzz_fastfile_path[256];

static void write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        abort();
    fwrite(data, 1, size, file);
    fclose(file);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (fuzz_cache_path[0] == '\0') {
        snprintf(fuzz_cache_path, sizeof(fuzz_cache_path), "tests/fuzz/out/aae_build.cache.%d", (int)getpid());
        snprintf(fuzz_fastfile_path, sizeof(fuzz_fastfile_path), "tests/fuzz/out/aae_build.ff.%d", (int)getpid());
    }
    aae_build_values values;
    memset(&values, 0, sizeof(values));
    aae_build_scan(data, size, &values);
    if (memchr(values.build_time, '\0', sizeof(values.build_time)) == NULL)
        abort();
    aae_build_time_valid((const char *)data, size);

    // the cache: the input as the file
    write_file(fuzz_cache_path, data, size);
    memset(&values, 0, sizeof(values));
    aae_build_cache_load(fuzz_cache_path, (long long)size, 0, &values);
    if (memchr(values.build_time, '\0', sizeof(values.build_time)) == NULL)
        abort();

    // the fastfile: "TAff" and the header, then the input as its blocks
    static uint8_t file[Fastfile_Header_Size + 65536];
    memset(file, 0, Fastfile_Header_Size);
    memcpy(file, "TAff", 4);
    size_t blocks = size < 65536 ? size : 65536;
    memcpy(file + Fastfile_Header_Size, data, blocks);
    write_file(fuzz_fastfile_path, file, Fastfile_Header_Size + blocks);
    memset(&values, 0, sizeof(values));
    aae_build_read_fastfile(fuzz_fastfile_path, &values);
    return 0;
}

void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    // a string constant with the changelist's name and a number after it, and the build time's
    static const uint8_t lua[] = {
        4, 26, 0, 0, 0, 0, 0, 0, 0, 'B', 'U', 'I', 'L', 'D', '_', 'I', 'N', 'T', 'F', 'I', 'E', 'L', 'D', '_', 'C',
        'H', 'A', 'N', 'G', 'E', 'L', 'I', 'S', 'T', 0, 3, 0x12, 0xfd, 0x53, 0x4b,
        4, 29, 0, 0, 0, 0, 0, 0, 0, 'B', 'U', 'I', 'L', 'D', '_', 'S', 'T', 'R', 'I', 'N', 'G', 'F', 'I', 'E', 'L',
        'D', '_', 'B', 'U', 'I', 'L', 'D', '_', 'T', 'I', 'M', 'E', 0,
        4, 25, 0, 0, 0, 0, 0, 0, 0, 'W', 'e', 'd', ' ', 'D', 'e', 'c', ' ', '1', '0', ' ', '1', '7', ':', '1', '0', ':',
        '4', '6', ' ', '2', '0', '2', '5', 0 };
    add(lua, sizeof(lua));
    static const char cache[] = "BO3MacFix aae_build.cache 1\nsize 99\nmtime 0\nchangelist 13892626\nbuildtime Wed Dec 10 "
                                "17:10:46 2025\n";
    add(cache, sizeof(cache) - 1);
    // a stored block at the fastfile's first block place: compressed, decompressed, size, its own offset
    static uint8_t block[16 + sizeof(lua)];
    uint32_t header[4] = { sizeof(lua), sizeof(lua), sizeof(lua), Fastfile_Header_Size };
    memcpy(block, header, sizeof(header));
    memcpy(block + 16, lua, sizeof(lua));
    add(block, sizeof(block));
}
void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    add("BUILD_INTFIELD_CHANGELIST", 26);
    add("BUILD_STRINGFIELD_BUILD_TIME", 29);
    add("\x04\x1a\0\0\0\0\0\0\0", 9);
    add("BO3MacFix aae_build.cache 1\n", 28);
    add("changelist ", 11);
    add("buildtime ", 10);
    add("\x78\x9c", 2);
}
