// Tests prepatch.c against a made-up executable with the game's layout: building the copy, the stubs it writes,
// whether a copy counts as up to date, damaged or cut-off originals, and the runtime side (hooks, settings and calls
// going into slots, the original code view). Nothing here touches the game.
#include <sys/mman.h>

#include "../source/prepatch.c"

static int failures = 0;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// what prepatch.c falls back to for hooks and patches the copy doesn't have
static int dobby_hooks = 0, dobby_patches = 0;
int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    (void)fake_func;
    dobby_hooks++;
    *out_origin_func = address;
    return 0;
}
int DobbyCodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {
    dobby_patches++;
    memcpy(address, buffer, buffer_size);
    return 0;
}

#define Data_File_Size 0x1000
#define File_Size (ADDR_Prepatch_CodeEnd + Data_File_Size)
#define Netver_At 3 // where the network version is after each of its addresses (patch_near searches 16 bytes)
#define Tuver_At 2

// the game's layout: __TEXT with its padding after the last section, __DATA whose zero-fill tail holds the slots
static uint8_t *make_original(void) {
    uint8_t *data = calloc(1, File_Size);
    struct mach_header_64 *mh = (struct mach_header_64 *)data;
    mh->magic = MH_MAGIC_64;
    mh->cputype = CPU_TYPE_X86_64;
    mh->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    mh->filetype = MH_EXECUTE;
    mh->ncmds = 2;
    mh->sizeofcmds = 2 * (sizeof(struct segment_command_64) + sizeof(struct section_64));
    struct segment_command_64 *text = (struct segment_command_64 *)(mh + 1);
    *text = (struct segment_command_64){ .cmd = LC_SEGMENT_64, .cmdsize = sizeof(*text) + sizeof(struct section_64),
                                         .segname = "__TEXT", .vmaddr = Game_Base, .vmsize = ADDR_Prepatch_CodeEnd,
                                         .fileoff = 0, .filesize = ADDR_Prepatch_CodeEnd, .maxprot = 5, .initprot = 5,
                                         .nsects = 1 };
    struct section_64 *text_section = (struct section_64 *)(text + 1);
    *text_section = (struct section_64){ .sectname = "__text", .segname = "__TEXT", .addr = Game_Base + 0x1000,
                                         .size = ADDR_Prepatch_Header - 0x1000, .offset = 0x1000 };
    struct segment_command_64 *seg = (struct segment_command_64 *)(text_section + 1);
    *seg = (struct segment_command_64){ .cmd = LC_SEGMENT_64, .cmdsize = sizeof(*seg) + sizeof(struct section_64),
                                        .segname = "__DATA", .vmaddr = Game_Base + ADDR_Prepatch_CodeEnd,
                                        .vmsize = ADDR_Prepatch_SlotsEnd - ADDR_Prepatch_CodeEnd,
                                        .fileoff = ADDR_Prepatch_CodeEnd, .filesize = Data_File_Size, .maxprot = 3,
                                        .initprot = 3, .nsects = 1 };
    struct section_64 *data_section = (struct section_64 *)(seg + 1);
    *data_section = (struct section_64){ .sectname = "__data", .segname = "__DATA", .addr = seg->vmaddr,
                                         .size = Data_File_Size, .offset = ADDR_Prepatch_CodeEnd };

    // the code everything expects, with something that isn't zero after each so the records' extra bytes show
    for (size_t i = 0; i < sizeof(hook_defs) / sizeof(hook_defs[0]); i++) {
        memset(data + hook_defs[i].function, 0xCC, 16);
        memcpy(data + hook_defs[i].function, hook_defs[i].stolen, hook_defs[i].length);
    }
    for (size_t i = 0; i < sizeof(value_defs) / sizeof(value_defs[0]); i++)
        memcpy(data + value_defs[i].site, value_defs[i].original, 5);
    for (size_t i = 0; i < sizeof(call_defs) / sizeof(call_defs[0]); i++)
        memcpy(data + call_defs[i].site, call_defs[i].original, 6);
    for (size_t i = 0; i < sizeof(patch_defs) / sizeof(patch_defs[0]); i++)
        memcpy(data + patch_defs[i].offset, patch_defs[i].original, patch_defs[i].length);
    for (size_t i = 0; i < sizeof(netver_addresses) / sizeof(netver_addresses[0]); i++)
        memcpy(data + netver_addresses[i] + Netver_At, &netver_original, sizeof(netver_original));
    for (size_t i = 0; i < sizeof(tuver_addresses) / sizeof(tuver_addresses[0]); i++)
        memcpy(data + tuver_addresses[i] + Tuver_At, tuver_original, sizeof(tuver_original));
    return data;
}

static char directory[256];

static void write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL || fwrite(data, 1, size, fp) != size)
        printf("FAIL: couldn't write %s\n", path);
    if (fp != NULL)
        fclose(fp);
}

static uint8_t *read_file(const char *path, size_t *size) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH], *data = NULL;
    if (sha256_file(path, digest, &data, size) != 0)
        return NULL;
    return data;
}

static int32_t rel32(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

// the copy's stubs, read back as the CPU would follow them
static void check_stubs(const uint8_t *copy) {
    uint32_t slot = ADDR_Prepatch_Slots;
    for (size_t i = 0; i < sizeof(hook_defs) / sizeof(hook_defs[0]); i++, slot += 8) {
        const hook_def_t *def = &hook_defs[i];
        const uint8_t *entry = copy + def->function;
        CHECK(entry[0] == 0xE9, "hook %08x doesn't start with a jmp", def->function);
        for (int n = 5; n < def->length; n++)
            CHECK(entry[n] == 0x90, "hook %08x: byte %i after the jmp isn't a nop", def->function, n);
        uint32_t stub = def->function + 5 + rel32(entry + 1);
        CHECK(stub >= ADDR_Prepatch_Header && stub + 32 <= ADDR_Prepatch_CodeEnd, "hook %08x: stub %08x isn't in the padding",
              def->function, stub);
        const uint8_t *s = copy + stub;
        CHECK(s[0] == 0x48 && s[1] == 0x83 && s[2] == 0x3D && s[7] == 0x00, "hook %08x: no cmp qword [rip+x], 0", def->function);
        CHECK(stub + 8 + rel32(s + 3) == slot, "hook %08x: cmp reads %08x, not its slot %08x", def->function,
              stub + 8 + rel32(s + 3), slot);
        CHECK(s[8] == 0x74 && s[9] == 0x06, "hook %08x: no je to the trampoline", def->function);
        CHECK(s[10] == 0xFF && s[11] == 0x25 && stub + 16 + rel32(s + 12) == slot, "hook %08x: no jmp [slot]", def->function);
        // the trampoline: the replaced instructions, then a jmp back after them
        const uint8_t *t = s + 16;
        size_t n;
        if (def->short_jcc_at != 0) {
            size_t at = def->short_jcc_at - 1;
            CHECK(memcmp(t, def->stolen, at) == 0, "hook %08x: trampoline doesn't start with the instructions", def->function);
            CHECK(t[at] == 0x0F && t[at + 1] == (0x80 | (def->stolen[at] & 0x0F)), "hook %08x: the jcc isn't near",
                  def->function);
            uint32_t target = stub + 16 + (uint32_t)at + 6 + rel32(t + at + 2);
            uint32_t original_target = def->function + (uint32_t)at + 2 + (int8_t)def->stolen[at + 1];
            CHECK(target == original_target, "hook %08x: the jcc goes to %08x, not %08x", def->function, target,
                  original_target);
            n = at + 6 + (def->length - at - 2);
        } else {
            CHECK(memcmp(t, def->stolen, def->length) == 0, "hook %08x: trampoline doesn't start with the instructions",
                  def->function);
            n = def->length;
        }
        uint32_t back = stub + 16 + (uint32_t)n + 5 + rel32(t + n + 1);
        CHECK(t[n] == 0xE9 && back == def->function + def->length, "hook %08x: the trampoline goes back to %08x",
              def->function, back);
    }
    for (size_t i = 0; i < sizeof(value_defs) / sizeof(value_defs[0]); i++, slot += 8) {
        const value_def_t *def = &value_defs[i];
        const uint8_t *site = copy + def->site;
        uint8_t reg = def->original[0] - 0xB8;
        CHECK(site[0] == 0xE9, "setting %08x isn't a jmp", def->site);
        uint32_t stub = def->site + 5 + rel32(site + 1);
        const uint8_t *s = copy + stub;
        CHECK(s[0] == 0x8B && s[1] == (0x05 | reg << 3) && stub + 6 + rel32(s + 2) == slot,
              "setting %08x: no mov r32, [slot]", def->site);
        CHECK(s[6] == 0x85 && s[7] == (0xC0 | reg << 3 | reg) && s[8] == 0x75 && s[9] == 0x05,
              "setting %08x: no test; jnz past the original", def->site);
        CHECK(memcmp(s + 10, def->original, 5) == 0, "setting %08x: the original mov isn't there", def->site);
        CHECK(s[15] == 0xE9 && stub + 20 + rel32(s + 16) == def->site + 5, "setting %08x: doesn't go back", def->site);
    }
    for (size_t i = 0; i < sizeof(call_defs) / sizeof(call_defs[0]); i++, slot += 8) {
        const call_def_t *def = &call_defs[i];
        const uint8_t *site = copy + def->site;
        CHECK(site[0] == 0xE8 && site[5] == 0x90, "call %08x isn't call stub; nop", def->site);
        uint32_t stub = def->site + 5 + rel32(site + 1);
        const uint8_t *s = copy + stub;
        CHECK(s[0] == 0x48 && s[1] == 0x83 && s[2] == 0x3D && stub + 8 + rel32(s + 3) == slot && s[8] == 0x74 &&
                  s[9] == 0x06 && s[10] == 0xFF && s[11] == 0x25 && stub + 16 + rel32(s + 12) == slot,
              "call %08x: no cmp/je/jmp [slot]", def->site);
        uint32_t pointer = def->site + 6 + rel32(def->original + 2);
        CHECK(s[16] == 0xFF && s[17] == 0x25 && stub + 22 + rel32(s + 18) == pointer,
              "call %08x: doesn't fall back to the call's own pointer", def->site);
    }
    for (size_t i = 0; i < sizeof(patch_defs) / sizeof(patch_defs[0]); i++) {
        const patch_def_t *def = &patch_defs[i];
        for (uint32_t n = 0; n < def->length; n++)
            CHECK(copy[def->offset + n] == (def->patched != NULL ? (uint8_t)def->patched[n] : 0x90),
                  "byte patch %08x isn't made", def->offset);
    }
}

// the copy as the game's image: its file-backed part, and zeros up to the end of __DATA
static uint8_t *map_image(const uint8_t *copy) {
    uint8_t *image = mmap(NULL, ADDR_Prepatch_SlotsEnd, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (image == MAP_FAILED)
        return NULL;
    memcpy(image, copy, File_Size);
    return image;
}

static void fake_function(void) {
}

static void check_runtime(uint8_t *image, const uint8_t *original) {
    uint64_t base = (uint64_t)image;
    CHECK(prepatch_init(base), "prepatch_init doesn't see the copy");
    CHECK(prepatch_active(), "not active");

    // the code as shipped, for every place the copy changed
    for (size_t i = 0; i < sizeof(hook_defs) / sizeof(hook_defs[0]); i++) {
        const void *at = image + hook_defs[i].function;
        CHECK(memcmp(prepatch_original_code(at, 16), original + hook_defs[i].function, 16) == 0,
              "original code of hook %08x", hook_defs[i].function);
    }
    for (size_t i = 0; i < sizeof(value_defs) / sizeof(value_defs[0]); i++)
        CHECK(memcmp(prepatch_original_code(image + value_defs[i].site, 5), value_defs[i].original, 5) == 0,
              "original code of setting %08x", value_defs[i].site);
    for (size_t i = 0; i < sizeof(call_defs) / sizeof(call_defs[0]); i++)
        CHECK(memcmp(prepatch_original_code(image + call_defs[i].site, 6), call_defs[i].original, 6) == 0,
              "original code of call %08x", call_defs[i].site);
    for (size_t i = 0; i < sizeof(patch_defs) / sizeof(patch_defs[0]); i++)
        CHECK(memcmp(prepatch_original_code(image + patch_defs[i].offset, patch_defs[i].length), patch_defs[i].original,
                     patch_defs[i].length) == 0, "original code of byte patch %08x", patch_defs[i].offset);
    uint8_t untouched[8] = { 0 };
    CHECK(prepatch_original_code(image + 0x2000, 8) == image + 0x2000 && memcmp(image + 0x2000, untouched, 8) == 0,
          "unchanged code isn't returned as it is");

    // hooks only set their slot, and the original is the trampoline
    uint32_t slot = ADDR_Prepatch_Slots;
    for (size_t i = 0; i < sizeof(hook_defs) / sizeof(hook_defs[0]); i++, slot += 8) {
        void *orig = NULL;
        CHECK(BO3MacFix_Hook(image + hook_defs[i].function, (void *)fake_function, &orig) == 0, "hook %08x refused",
              hook_defs[i].function);
        CHECK(*(void **)(image + slot) == (void *)fake_function, "hook %08x: slot not set", hook_defs[i].function);
        uint32_t stub = hook_defs[i].function + 5 + rel32(image + hook_defs[i].function + 1);
        CHECK(orig == image + stub + 16, "hook %08x: the original isn't the trampoline", hook_defs[i].function);
    }
    // a value patch goes into the setting's slot, a byte patch already in the copy is left alone
    uint8_t mov_edx_4[] = { 0xBA, 0x04, 0x00, 0x00, 0x00 };
    CHECK(BO3MacFix_CodePatch(image + ADDR_JobThreadsDvar_DefaultInstr, mov_edx_4, sizeof(mov_edx_4)) == 0, "JobThreads");
    CHECK(*(uint32_t *)(image + slot) == 4, "JobThreads' slot is %u", *(uint32_t *)(image + slot));
    CHECK(prepatch_set_value(ADDR_RenderThread_WaitTimeoutInstr, 2) && *(uint32_t *)(image + slot + 16) == 2,
          "RenderIdleFix's slot");
    CHECK(prepatch_set_value(ADDR_RenderThread_WaitTimeoutInstr, 0) && *(uint32_t *)(image + slot + 16) == 0,
          "RenderIdleFix off");
    CHECK(!prepatch_set_value(ADDR_RenderThread_WaitTimeoutInstr + 1, 2), "a site that isn't one");
    CHECK(prepatch_redirect_call(ADDR_ASL_Present_PresentDrawableCall, (void *)fake_function) &&
              *(void **)(image + slot + 4 * 8) == (void *)fake_function, "present's slot");
    uint8_t ret[] = { 0xC3 };
    int in_place = patches_in_place;
    CHECK(BO3MacFix_CodePatch(image + ADDR_Lua_CmdParseArgs, ret, 1) == 0 && patches_in_place == in_place + 1,
          "a byte patch already made");

    // nothing so far wrote code; a hook or patch the copy doesn't have is done in memory, unless the copy changed
    // that code
    CHECK(dobby_hooks == 0 && dobby_patches == 0, "code was written (%i hooks, %i patches)", dobby_hooks, dobby_patches);
    void *orig = NULL;
    CHECK(BO3MacFix_Hook(image + 0x3000, (void *)fake_function, &orig) == 0 && dobby_hooks == 1, "a new hook in memory");
    CHECK(BO3MacFix_Hook(image + hook_defs[0].function + 1, (void *)fake_function, &orig) == -1 && dobby_hooks == 1,
          "a hook inside a changed instruction isn't refused");
    uint8_t other[] = { image[ADDR_JQ_IdleWait_TimeoutInstr + 1] ^ 0xFF };
    CHECK(BO3MacFix_CodePatch(image + ADDR_JQ_IdleWait_TimeoutInstr + 1, other, 1) == -1 && dobby_patches == 0,
          "a patch inside a setting's jmp isn't refused");
    // longer than prepatch_original_code can show, over a hook's jmp
    uint8_t long_patch[100];
    memset(long_patch, 0x90, sizeof(long_patch));
    CHECK(BO3MacFix_CodePatch(image + hook_defs[1].function - 40, long_patch, sizeof(long_patch)) == -1 &&
              dobby_patches == 0, "a long patch over a hook isn't refused");
    // right after a hook's jmp, which the copy didn't change
    CHECK(BO3MacFix_Hook(image + hook_defs[1].function + hook_defs[1].length + 16, (void *)fake_function, &orig) == 0 &&
              dobby_hooks == 2, "a hook just after a changed one is refused");
    uint8_t nop[] = { 0x90 };
    CHECK(BO3MacFix_CodePatch(image + 0x3100, nop, 1) == 0 && dobby_patches == 1, "a new patch in memory");
    // __DATA is written directly
    uint8_t value[] = { 1, 2, 3, 4 };
    CHECK(BO3MacFix_CodePatch(image + ADDR_Prepatch_CodeEnd + 0x10, value, 4) == 0 && dobby_patches == 1 &&
              memcmp(image + ADDR_Prepatch_CodeEnd + 0x10, value, 4) == 0, "a data patch");
}

static void expect_failure(const char *what, const char *original, const char *copy, const char *reason) {
    char message[512] = "";
    int result = BO3MacFix_BuildPrepatched(original, copy, message, sizeof(message));
    CHECK(result == -1 && strstr(message, reason) != NULL, "%s: %i, \"%s\"", what, result, message);
    CHECK(access(copy, F_OK) != 0, "%s left a copy behind", what);
}

int main(void) {
    snprintf(directory, sizeof(directory), "%s/bo3mf_prepatch_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (mkdtemp(directory) == NULL) {
        printf("FAIL: no temporary directory\n");
        return 1;
    }
    char original_path[300], copy_path[300], other_path[300];
    snprintf(original_path, sizeof(original_path), "%s/CoDBlkOps3_Exe", directory);
    snprintf(copy_path, sizeof(copy_path), "%s/CoDBlkOps3_Exe.bo3macfix", directory);
    snprintf(other_path, sizeof(other_path), "%s/other", directory);
    char message[512];

    uint8_t *original = make_original();
    write_file(original_path, original, File_Size);
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == -1 &&
              strstr(message, "no pre-patched") != NULL, "no copy yet: \"%s\"", message);
    CHECK(BO3MacFix_BuildPrepatched(original_path, copy_path, message, sizeof(message)) == 0, "build: \"%s\"", message);
    printf("%s\n", message);
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == 0, "current: \"%s\"", message);
    size_t copy_size = 0;
    uint8_t *copy = read_file(copy_path, &copy_size);
    CHECK(copy != NULL && copy_size == File_Size, "the copy is %zu bytes", copy_size);
    struct stat st;
    CHECK(stat(copy_path, &st) == 0 && (st.st_mode & 0777) == 0755, "the copy's mode is %o", st.st_mode & 0777);
    if (copy != NULL) {
        check_stubs(copy);
        // outside what it changed, the copy is the original
        size_t differences = 0;
        for (size_t i = 0; i < File_Size; i++)
            differences += copy[i] != original[i];
        CHECK(differences < 4096, "%zu bytes differ", differences);
        uint8_t *image = map_image(copy);
        CHECK(image != NULL, "no memory for the image");
        if (image != NULL) {
            check_runtime(image, original);
            munmap(image, ADDR_Prepatch_SlotsEnd);
        }
    }

    // a game update: the copy no longer matches the original
    original[0x5000] ^= 1;
    write_file(original_path, original, File_Size);
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == -1 &&
              strstr(message, "updated") != NULL, "after an update: \"%s\"", message);
    original[0x5000] ^= 1;
    write_file(original_path, original, File_Size);

    // a copy from another BO3MacFix
    if (copy != NULL) {
        ((prepatch_header_t *)(copy + ADDR_Prepatch_Header))->table_id ^= 1;
        write_file(other_path, copy, copy_size);
        CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, other_path, message, sizeof(message)) == -1 &&
                  strstr(message, "different BO3MacFix") != NULL, "another BO3MacFix's copy: \"%s\"", message);
        ((prepatch_header_t *)(copy + ADDR_Prepatch_Header))->version = 1;
        write_file(other_path, copy, copy_size);
        CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, other_path, message, sizeof(message)) == -1 &&
                  strstr(message, "older") != NULL, "an older copy: \"%s\"", message);
        unlink(other_path);
    }

    // originals the copy can't be made from, which must fail without reading past the file
    unlink(copy_path);
    uint8_t saved = original[hook_defs[3].function + 2];
    original[hook_defs[3].function + 2] ^= 0xFF;
    write_file(other_path, original, File_Size);
    expect_failure("changed code", other_path, copy_path, "unexpected code");
    original[hook_defs[3].function + 2] = saved;
    original[ADDR_Prepatch_Header + 0x100] = 1;
    write_file(other_path, original, File_Size);
    expect_failure("padding in use", other_path, copy_path, "isn't empty");
    original[ADDR_Prepatch_Header + 0x100] = 0;
    write_file(other_path, original, ADDR_Prepatch_Header - 0x100000);
    expect_failure("cut off in __TEXT", other_path, copy_path, "shorter");
    write_file(other_path, original, File_Size - 0x10);
    expect_failure("cut off in __DATA", other_path, copy_path, "shorter");
    write_file(other_path, original, 0x800);
    expect_failure("only the header", other_path, copy_path, "shorter");
    write_file(other_path, original, 16);
    expect_failure("not even a header", other_path, copy_path, "isn't the x86_64 game executable");
    ((struct segment_command_64 *)((struct mach_header_64 *)original + 1))->nsects = 1000;
    write_file(other_path, original, File_Size);
    expect_failure("damaged load commands", other_path, copy_path, "damaged");
    expect_failure("no original", "/nonexistent/CoDBlkOps3_Exe", copy_path, "couldn't read");

    unlink(other_path);
    unlink(original_path);
    rmdir(directory);
    free(copy);
    free(original);
    if (failures != 0) {
        printf("prepatch: %i failures\n", failures);
        return 1;
    }
    printf("prepatch: all tests passed\n");
    return 0;
}
