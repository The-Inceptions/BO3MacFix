// utilities.c's file and text helpers: whether a file is a Mac binary (asked at every Lua loadlib, so it mustn't keep
// files open), a hash's bytes as hex, and a path's file name.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/utilities.c"

// what utilities.c asks of the rest (replace_vtable_entry's), never called here
int DobbyCodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {
    (void)address, (void)buffer, (void)buffer_size;
    return 0;
}

static void test_dylib_check(const char *self) {
    char dir[] = "/tmp/bo3macfix-utilities-test-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        printf("FAIL: no folder for the test\n");
        failures++;
        return;
    }
    char text[sizeof(dir) + 16], short_file[sizeof(dir) + 16];
    snprintf(text, sizeof(text), "%s/a.dll", dir);
    snprintf(short_file, sizeof(short_file), "%s/b.ff", dir);
    FILE *file = fopen(text, "wb");
    fputs("MZ\x90\0 a Windows DLL", file);
    fclose(file);
    file = fopen(short_file, "wb");
    fputs("\xcf\xfa", file);
    fclose(file);
    CHECK(is_macos_dylib(self)); // this test is a Mach-O
    CHECK(!is_macos_dylib(text));
    CHECK(!is_macos_dylib(short_file));
    CHECK(!is_macos_dylib("/no/such/file"));
    // asked again and again, as every loadlib asks, with few files allowed: none may stay open
    struct rlimit limit;
    getrlimit(RLIMIT_NOFILE, &limit);
    struct rlimit few = limit;
    few.rlim_cur = 32;
    if (setrlimit(RLIMIT_NOFILE, &few) == 0) {
        int yes = 0;
        for (int i = 0; i < 200; i++)
            yes += is_macos_dylib(self) && !is_macos_dylib(text);
        CHECK(yes == 200);
        setrlimit(RLIMIT_NOFILE, &limit);
    }
    unlink(text);
    unlink(short_file);
    rmdir(dir);
}

static void test_hex(void) {
    const uint8_t bytes[20] = { 0x00, 0x01, 0xab, 0xff, 0x10, 0x9c, 0x7f, 0x80, 0x00, 0x00,
                                0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xcd };
    char hex[41];
    memset(hex, 'x', sizeof(hex));
    CHECK(bytes_to_hex(bytes, sizeof(bytes), hex, sizeof(hex)));
    CHECK(strcmp(hex, "0001abff109c7f800000deadbeef0123456789cd") == 0);
    CHECK(!bytes_to_hex(bytes, sizeof(bytes), hex, 40)); // no room for the end
    CHECK(bytes_to_hex(bytes, 0, hex, 1) && hex[0] == '\0');
}

static void test_file_name(void) {
    CHECK(strcmp(filename_from_path("../../../BO3MacFix/natives/T7Overcharged.dll"), "T7Overcharged.dll") == 0);
    CHECK(strcmp(filename_from_path("T7Overcharged.ff"), "T7Overcharged.ff") == 0);
    CHECK(strcmp(filename_from_path("a/"), "") == 0);
    CHECK(strcmp(filename_from_path("/b"), "/b") == 0); // (the first character isn't looked at, as before)
    // an empty path: there's no last character to start from
    const char *empty = "";
    CHECK(filename_from_path(empty) == empty);
}

int main(int argc, char **argv) {
    (void)argc;
    test_dylib_check(argv[0]);
    test_hex();
    test_file_name();
    if (failures != 0) {
        printf("test_utilities: %d failed\n", failures);
        return 1;
    }
    printf("test_utilities: all passed\n");
    return 0;
}
