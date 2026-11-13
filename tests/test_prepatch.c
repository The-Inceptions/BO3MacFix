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

// the game's layout: __TEXT with padding before its first section and after its last, __DATA whose zero-fill tail holds
// the slots
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
    *text_section = (struct section_64){ .sectname = "__text", .segname = "__TEXT",
                                         .addr = Game_Base + ADDR_Prepatch_Stubs2End,
                                         .size = ADDR_Prepatch_Header - ADDR_Prepatch_Stubs2End,
                                         .offset = ADDR_Prepatch_Stubs2End };
    struct segment_command_64 *seg = (struct segment_command_64 *)(text_section + 1);
    *seg = (struct segment_command_64){ .cmd = LC_SEGMENT_64, .cmdsize = sizeof(*seg) + sizeof(struct section_64),
                                        .segname = "__DATA", .vmaddr = Game_Base + ADDR_Prepatch_CodeEnd,
                                        .vmsize = ADDR_Prepatch_SlotsEnd - ADDR_Prepatch_CodeEnd,
                                        .fileoff = ADDR_Prepatch_CodeEnd, .filesize = Data_File_Size, .maxprot = 3,
                                        .initprot = 3, .nsects = 1 };
    struct section_64 *data_section = (struct section_64 *)(seg + 1);
    *data_section = (struct section_64){ .sectname = "__data", .segname = "__DATA", .addr = seg->vmaddr,
                                         .size = Data_File_Size, .offset = ADDR_Prepatch_CodeEnd };

    // the code everything expects, with something that isn't zero after each, so that a view of more than the change
    // shows it
    for (size_t i = 0; i < Hook_Count; i++) {
        memset(data + hook_defs[i].function, 0xCC, 16);
        memcpy(data + hook_defs[i].function, hook_defs[i].stolen, hook_defs[i].length);
    }
    for (size_t i = 0; i < Value_Count; i++)
        memcpy(data + value_defs[i].site, value_defs[i].original, 5);
    for (size_t i = 0; i < Call_Count; i++)
        memcpy(data + call_defs[i].site, call_defs[i].original, 6);
    for (size_t i = 0; i < Patch_Count; i++)
        memcpy(data + patch_defs[i].offset, patch_defs[i].original, patch_defs[i].length);
    for (size_t i = 0; i < Netver_Count; i++)
        memcpy(data + netver_addresses[i] + Netver_At, &netver_original, sizeof(netver_original));
    for (size_t i = 0; i < Tuver_Count; i++)
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

static int stubs_before_code;         // how many of the last copy's stubs are in the padding before __text
static uint32_t last_stub_after_code; // and where the last one of the others is

static bool in_padding(uint32_t stub) {
    if (stub >= ADDR_Prepatch_Stubs2 && stub + Stub_Max <= ADDR_Prepatch_Stubs2End) {
        stubs_before_code++;
        return true;
    }
    if (stub < ADDR_Prepatch_Header || stub + Stub_Max > first_area_end)
        return false;
    if (stub > last_stub_after_code)
        last_stub_after_code = stub;
    return true;
}

// the copy's stubs, read back as the CPU would follow them
static void check_stubs(const uint8_t *copy) {
    uint32_t slot = ADDR_Prepatch_Slots;
    stubs_before_code = 0;
    last_stub_after_code = 0;
    for (size_t i = 0; i < Hook_Count; i++, slot += 8) {
        const hook_def_t *def = &hook_defs[i];
        const uint8_t *entry = copy + def->function;
        CHECK(entry[0] == 0xE9, "hook %08x doesn't start with a jmp", def->function);
        for (int n = 5; n < def->length; n++)
            CHECK(entry[n] == 0x90, "hook %08x: byte %i after the jmp isn't a nop", def->function, n);
        uint32_t stub = def->function + 5 + rel32(entry + 1);
        CHECK(in_padding(stub), "hook %08x: stub %08x isn't in the padding", def->function, stub);
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
    for (size_t i = 0; i < Value_Count; i++, slot += 8) {
        const value_def_t *def = &value_defs[i];
        const uint8_t *site = copy + def->site;
        uint8_t reg = def->original[0] - 0xB8;
        CHECK(site[0] == 0xE9, "setting %08x isn't a jmp", def->site);
        uint32_t stub = def->site + 5 + rel32(site + 1);
        const uint8_t *s = copy + stub;
        CHECK(in_padding(stub), "setting %08x: stub %08x isn't in the padding", def->site, stub);
        CHECK(s[0] == 0x8B && s[1] == (0x05 | reg << 3) && stub + 6 + rel32(s + 2) == slot,
              "setting %08x: no mov r32, [slot]", def->site);
        CHECK(s[6] == 0x85 && s[7] == (0xC0 | reg << 3 | reg) && s[8] == 0x75 && s[9] == 0x05,
              "setting %08x: no test; jnz past the original", def->site);
        CHECK(memcmp(s + 10, def->original, 5) == 0, "setting %08x: the original mov isn't there", def->site);
        CHECK(s[15] == 0xE9 && stub + 20 + rel32(s + 16) == def->site + 5, "setting %08x: doesn't go back", def->site);
    }
    for (size_t i = 0; i < Call_Count; i++, slot += 8) {
        const call_def_t *def = &call_defs[i];
        const uint8_t *site = copy + def->site;
        CHECK(site[0] == 0xE8 && site[5] == 0x90, "call %08x isn't call stub; nop", def->site);
        uint32_t stub = def->site + 5 + rel32(site + 1);
        const uint8_t *s = copy + stub;
        CHECK(in_padding(stub), "call %08x: stub %08x isn't in the padding", def->site, stub);
        CHECK(s[0] == 0x48 && s[1] == 0x83 && s[2] == 0x3D && stub + 8 + rel32(s + 3) == slot && s[8] == 0x74 &&
                  s[9] == 0x06 && s[10] == 0xFF && s[11] == 0x25 && stub + 16 + rel32(s + 12) == slot,
              "call %08x: no cmp/je/jmp [slot]", def->site);
        uint32_t pointer = def->site + 6 + rel32(def->original + 2);
        CHECK(s[16] == 0xFF && s[17] == 0x25 && stub + 22 + rel32(s + 18) == pointer,
              "call %08x: doesn't fall back to the call's own pointer", def->site);
    }
    for (size_t i = 0; i < Patch_Count; i++) {
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
    for (size_t i = 0; i < Hook_Count; i++) {
        const void *at = image + hook_defs[i].function;
        CHECK(memcmp(prepatch_original_code(at, 16), original + hook_defs[i].function, 16) == 0,
              "original code of hook %08x", hook_defs[i].function);
    }
    for (size_t i = 0; i < Value_Count; i++)
        CHECK(memcmp(prepatch_original_code(image + value_defs[i].site, 5), value_defs[i].original, 5) == 0,
              "original code of setting %08x", value_defs[i].site);
    for (size_t i = 0; i < Call_Count; i++)
        CHECK(memcmp(prepatch_original_code(image + call_defs[i].site, 6), call_defs[i].original, 6) == 0,
              "original code of call %08x", call_defs[i].site);
    for (size_t i = 0; i < Patch_Count; i++)
        CHECK(memcmp(prepatch_original_code(image + patch_defs[i].offset, patch_defs[i].length), patch_defs[i].original,
                     patch_defs[i].length) == 0, "original code of byte patch %08x", patch_defs[i].offset);
    // the version patches aren't at their addresses but a few bytes behind them, as far as the header says
    for (size_t i = 0; i < Netver_Count; i++) {
        const uint8_t *at = image + netver_addresses[i] + Netver_At;
        CHECK(memcmp(at, &netver_patched, sizeof(netver_patched)) == 0 &&
                  memcmp(prepatch_original_code(at, sizeof(netver_original)), &netver_original, sizeof(netver_original)) == 0,
              "original code of the network version at %08x", netver_addresses[i]);
        // (as network_version_patch looks for it: from the address on, 16 bytes at most)
        int found = 0;
        while (found < 16 && memcmp(prepatch_original_code(image + netver_addresses[i] + found, sizeof(netver_original)),
                                    &netver_original, sizeof(netver_original)) != 0)
            found++;
        CHECK(found == Netver_At, "the network version at %08x is found %i bytes behind it", netver_addresses[i], found);
    }
    for (size_t i = 0; i < Tuver_Count; i++) {
        const uint8_t *at = image + tuver_addresses[i] + Tuver_At;
        CHECK(memcmp(at, tuver_patched, sizeof(tuver_patched)) == 0 &&
                  memcmp(prepatch_original_code(at, sizeof(tuver_original)), tuver_original, sizeof(tuver_original)) == 0,
              "original code of the version string at %08x", tuver_addresses[i]);
    }
    uint8_t untouched[8] = { 0 };
    CHECK(prepatch_original_code(image + 0x2800, 8) == image + 0x2800 && memcmp(image + 0x2800, untouched, 8) == 0,
          "unchanged code isn't returned as it is");
    // a view that only touches a change's last byte, or begins in its middle, has that part as shipped
    const hook_def_t *first_hook = &hook_defs[0];
    const uint8_t *around = prepatch_original_code(image + first_hook->function - 4, 8);
    CHECK(memcmp(around, original + first_hook->function - 4, 8) == 0, "original code from before a hook into it");
    around = prepatch_original_code(image + first_hook->function + first_hook->length - 1, 8);
    CHECK(memcmp(around, original + first_hook->function + first_hook->length - 1, 8) == 0,
          "original code from a hook's last byte on");

    // hooks only set their slot, and the original is the trampoline
    uint32_t slot = ADDR_Prepatch_Slots;
    for (size_t i = 0; i < Hook_Count; i++, slot += 8) {
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
    CHECK(prepatch_set_value(ADDR_Main_RenderWait_TimeoutInstr, 2) && *(uint32_t *)(image + slot + 32) == 2,
          "MainWaitFix's slot");
    CHECK(!prepatch_set_value(ADDR_RenderThread_WaitTimeoutInstr + 1, 2), "a site that isn't one");
    CHECK(prepatch_redirect_call(ADDR_ASL_Present_PresentDrawableCall, (void *)fake_function) &&
              *(void **)(image + slot + 5 * 8) == (void *)fake_function, "present's slot");
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
    // what the copy changed ends with its last byte and begins with its first: the bytes next to them are free
    const hook_def_t *edge = &hook_defs[2];
    uint8_t first[] = { image[edge->function] ^ 0xFF }, last[] = { image[edge->function + edge->length - 1] ^ 0xFF };
    CHECK(BO3MacFix_CodePatch(image + edge->function, first, 1) == -1 &&
              BO3MacFix_CodePatch(image + edge->function + edge->length - 1, last, 1) == -1 && dobby_patches == 1,
          "a patch of a hook's first or last byte isn't refused");
    uint8_t before[] = { image[edge->function - 1] ^ 0xFF }, behind[] = { image[edge->function + edge->length] ^ 0xFF };
    CHECK(BO3MacFix_CodePatch(image + edge->function - 1, before, 1) == 0 &&
              BO3MacFix_CodePatch(image + edge->function + edge->length, behind, 1) == 0 && dobby_patches == 3,
          "a patch next to a hook is refused");
    // (the network version's second byte is the same before and after, and still the copy's)
    uint8_t *second_byte = image + netver_addresses[0] + Netver_At + 1;
    uint8_t other_half[] = { *second_byte ^ 0xFF };
    CHECK(BO3MacFix_CodePatch(second_byte, other_half, 1) == -1 && dobby_patches == 3,
          "a patch of the network version's second byte isn't refused");
    // a setting's place only takes that setting's own "mov r32, imm32": anything else there would be written over
    // the copy's jump
    const value_def_t *setting = &value_defs[1];
    uint32_t setting_stub = setting->site + 5 + rel32(image + setting->site + 1);
    uint32_t *setting_slot = (uint32_t *)(image + setting_stub + 6 + rel32(image + setting_stub + 2));
    uint32_t slot_before = *setting_slot;
    uint8_t other_register[5] = { setting->original[0] ^ 1, 7, 0, 0, 0 }, opcode_only[1] = { setting->original[0] };
    CHECK(BO3MacFix_CodePatch(image + setting->site, other_register, 5) == -1 &&
              BO3MacFix_CodePatch(image + setting->site, opcode_only, 1) == -1 && *setting_slot == slot_before &&
              dobby_patches == 3, "a setting's place takes something that isn't its mov");
    // __DATA is written directly
    uint8_t value[] = { 1, 2, 3, 4 };
    CHECK(BO3MacFix_CodePatch(image + ADDR_Prepatch_CodeEnd + 0x10, value, 4) == 0 && dobby_patches == 3 &&
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
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == 0 &&
              strstr(message, "unchanged") == NULL, "current: \"%s\"", message);
    // the original's hash is kept, so the next launch doesn't read the whole file again
    char checked_path[300];
    snprintf(checked_path, sizeof(checked_path), "%s.checked", copy_path);
    CHECK(access(checked_path, F_OK) == 0, "no %s after a check", checked_path);
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == 0 &&
              strstr(message, "unchanged") != NULL, "current from the kept hash: \"%s\"", message);
    size_t copy_size = 0;
    uint8_t *copy = read_file(copy_path, &copy_size);
    CHECK(copy != NULL && copy_size == File_Size, "the copy is %zu bytes", copy_size);
    struct stat st;
    CHECK(stat(copy_path, &st) == 0 && (st.st_mode & 0777) == 0755, "the copy's mode is %o", st.st_mode & 0777);
    if (copy != NULL) {
        check_stubs(copy);
        // outside what it changed, the copy is the original: the two paddings, and the places its tables name
        uint8_t *may_differ = calloc(1, File_Size);
        memset(may_differ + ADDR_Prepatch_Header, 1, ADDR_Prepatch_CodeEnd - ADDR_Prepatch_Header);
        memset(may_differ + ADDR_Prepatch_Stubs2, 1, ADDR_Prepatch_Stubs2End - ADDR_Prepatch_Stubs2);
        for (size_t i = 0; i < Hook_Count; i++)
            memset(may_differ + hook_defs[i].function, 1, hook_defs[i].length);
        for (size_t i = 0; i < Value_Count; i++)
            memset(may_differ + value_defs[i].site, 1, 5);
        for (size_t i = 0; i < Call_Count; i++)
            memset(may_differ + call_defs[i].site, 1, 6);
        for (size_t i = 0; i < Patch_Count; i++)
            memset(may_differ + patch_defs[i].offset, 1, patch_defs[i].length);
        for (size_t i = 0; i < Netver_Count; i++)
            memset(may_differ + netver_addresses[i] + Netver_At, 1, sizeof(netver_original));
        for (size_t i = 0; i < Tuver_Count; i++)
            memset(may_differ + tuver_addresses[i] + Tuver_At, 1, sizeof(tuver_original));
        size_t differences = 0, elsewhere = 0, first_elsewhere = 0;
        for (size_t i = 0; i < File_Size; i++) {
            if (copy[i] == original[i])
                continue;
            differences++;
            if (!may_differ[i] && elsewhere++ == 0)
                first_elsewhere = i;
        }
        free(may_differ);
        CHECK(elsewhere == 0, "%zu bytes differ outside what the copy may change, the first at %08zx", elsewhere,
              first_elsewhere);
        CHECK(differences > 1000, "only %zu bytes differ", differences);
        uint8_t *image = map_image(copy);
        CHECK(image != NULL, "no memory for the image");
        if (image != NULL) {
            check_runtime(image, original);
            munmap(image, ADDR_Prepatch_SlotsEnd);
        }
        // the header is all the copy says about itself: no list of what it changed takes room from the stubs. Its
        // first two fields stay where every format had them, so that any BO3MacFix can tell a copy's format.
        const prepatch_header_t *made = (const prepatch_header_t *)(copy + ADDR_Prepatch_Header);
        CHECK(sizeof(prepatch_header_t) <= 96 && offsetof(prepatch_header_t, magic) == 0 &&
                  offsetof(prepatch_header_t, version) == 8, "the header is %zu bytes", sizeof(prepatch_header_t));
        CHECK(memcmp(made->magic, Prepatch_Magic, 8) == 0 && made->version == Prepatch_Version &&
                  made->table_id == table_id(), "the header's magic, version or table_id");
        for (size_t i = 0; i < Netver_Count + Tuver_Count; i++)
            CHECK(made->version_patch_at[i] == (i < Netver_Count ? Netver_At : Tuver_At),
                  "the header has version patch %zu %u bytes behind its address", i, made->version_patch_at[i]);
        // the first stub follows the header directly, and what the header's fields leave unused of it is zero
        CHECK(copy[ADDR_Prepatch_Header + sizeof(prepatch_header_t)] == 0x48, "no stub right behind the header");
        size_t fields_end = offsetof(prepatch_header_t, version_patch_at) + Netver_Count + Tuver_Count;
        for (size_t i = fields_end; i < sizeof(prepatch_header_t); i++)
            CHECK(copy[ADDR_Prepatch_Header + i] == 0, "byte %zu of the header isn't zero", i);
        // building it again gives the same file, byte for byte
        CHECK(BO3MacFix_BuildPrepatched(original_path, other_path, message, sizeof(message)) == 0, "build: \"%s\"", message);
        size_t again_size = 0;
        uint8_t *again = read_file(other_path, &again_size);
        CHECK(again != NULL && again_size == copy_size && memcmp(again, copy, copy_size) == 0,
              "the same original gave another copy the second time");
        free(again);
        unlink(other_path);
        // A copy whose code at a hooked function or in its stub isn't what the stubs look like: that hook is refused,
        // nothing is written anywhere, and the others still work
        image = map_image(copy);
        if (image != NULL) {
            CHECK(prepatch_init((uint64_t)image), "prepatch_init doesn't see the copy");
            const uint32_t slots_size = ADDR_Prepatch_SlotsEnd - ADDR_Prepatch_Slots;
            uint8_t *no_slots = calloc(1, slots_size);
            int32_t far_away = 0x100000;
            // 0: the function doesn't start with a jump; 1: the jump goes somewhere that isn't the stubs' room;
            // 2: the stub doesn't start with the cmp; 3: nor go on with the je; 4: its slot isn't among the slots;
            // 6: its jump isn't one through a slot; 7: or goes through another slot than the cmp looks at;
            // 8: the function's jump goes to just below the image (where nothing is: reading there would end this
            // test), which a sum that wraps around takes for the room's end; 9: its slot begins 7 bytes before the
            // slots' end, where a pointer no longer fits
            const int damaged[] = { 0, 1, 2, 3, 4, 6, 7, 8, 9 };
            uint32_t stub_of[10];
            for (int i = 0; i < 10; i++)
                stub_of[i] = hook_defs[i].function + 5 + rel32(image + hook_defs[i].function + 1);
            image[hook_defs[0].function] = 0xE8;
            // (what the jump goes to there looks like a stub, with the first slot as its own)
            uint32_t elsewhere = hook_defs[1].function + 5 + (uint32_t)far_away;
            const uint8_t like_a_stub[16] = { 0x48, 0x83, 0x3D, 0, 0, 0, 0, 0x00, 0x74, 0x06, 0xFF, 0x25 };
            int32_t to_a_slot = (int32_t)(ADDR_Prepatch_Slots - (elsewhere + 8));
            int32_t to_the_slot_again = (int32_t)(ADDR_Prepatch_Slots - (elsewhere + 16));
            memcpy(image + elsewhere, like_a_stub, sizeof(like_a_stub));
            memcpy(image + elsewhere + 3, &to_a_slot, sizeof(to_a_slot));
            memcpy(image + elsewhere + 12, &to_the_slot_again, sizeof(to_the_slot_again));
            memcpy(image + hook_defs[1].function + 1, &far_away, sizeof(far_away));
            image[stub_of[2] + 2] = 0x3C;
            image[stub_of[3] + 9] = 0x07;
            int32_t far_away_too = far_away - 8; // (the same far place for the stub's jump)
            memcpy(image + stub_of[4] + 3, &far_away, sizeof(far_away));
            memcpy(image + stub_of[4] + 12, &far_away_too, sizeof(far_away_too));
            image[stub_of[6] + 11] = 0x15;
            int32_t next_slot = rel32(image + stub_of[7] + 12) + 8;
            memcpy(image + stub_of[7] + 12, &next_slot, sizeof(next_slot));
            int32_t below_the_image = (int32_t)(0xfffffff0u - (hook_defs[8].function + 5));
            memcpy(image + hook_defs[8].function + 1, &below_the_image, sizeof(below_the_image));
            int32_t past_the_slots = (int32_t)(ADDR_Prepatch_SlotsEnd - 7 - (stub_of[9] + 8));
            int32_t past_the_slots_too = past_the_slots - 8;
            memcpy(image + stub_of[9] + 3, &past_the_slots, sizeof(past_the_slots));
            memcpy(image + stub_of[9] + 12, &past_the_slots_too, sizeof(past_the_slots_too));
            dobby_hooks = 0;
            int refused_before = refused;
            for (size_t i = 0; i < sizeof(damaged) / sizeof(damaged[0]); i++) {
                void *orig = (void *)&failures;
                CHECK(BO3MacFix_Hook(image + hook_defs[damaged[i]].function, (void *)fake_function, &orig) == -1 &&
                          orig == (void *)&failures, "hook %i of a damaged copy isn't refused", damaged[i]);
            }
            CHECK(refused == refused_before + 9 && dobby_hooks == 0, "%i refused, %i hooked in memory",
                  refused - refused_before, dobby_hooks);
            CHECK(memcmp(image + ADDR_Prepatch_Slots, no_slots, slots_size) == 0, "a refused hook set a slot");
            void *orig = NULL;
            CHECK(BO3MacFix_Hook(image + hook_defs[5].function, (void *)fake_function, &orig) == 0 && orig != NULL,
                  "the hook after the damaged ones");
            // a room's very last bytes are in it, the byte behind them isn't, and nothing wraps around
            CHECK(in_stub_room(ADDR_Prepatch_CodeEnd - 16, 16) && !in_stub_room(ADDR_Prepatch_CodeEnd - 15, 16) &&
                      in_stub_room(ADDR_Prepatch_Stubs2, 16) && in_stub_room(ADDR_Prepatch_Stubs2End - 16, 16) &&
                      !in_stub_room(ADDR_Prepatch_Stubs2End - 15, 16) && !in_stub_room(ADDR_Prepatch_Stubs2 - 1, 16) &&
                      !in_stub_room(ADDR_Prepatch_Header - 1, 16) && !in_stub_room(0xfffffff8u, 16) &&
                      !in_stub_room(0xfffffff0u, 16), "the stubs' room");
            // A setting or a call whose stub isn't as expected isn't set either: the setting's patch is refused (the
            // copy changed that code, so nothing may be written there), the call says no, and no slot is written
            const value_def_t *setting = &value_defs[0], *other_setting = &value_defs[1], *third_setting = &value_defs[2];
            uint32_t setting_stub = setting->site + 5 + rel32(image + setting->site + 1);
            uint32_t other_stub = other_setting->site + 5 + rel32(image + other_setting->site + 1);
            uint32_t third_stub = third_setting->site + 5 + rel32(image + third_setting->site + 1);
            uint8_t third_wanted[5] = { third_setting->original[0], 9, 0, 0, 0 };
            uint32_t call_stub = call_defs[0].site + 5 + rel32(image + call_defs[0].site + 1);
            uint8_t wanted[5] = { setting->original[0], 9, 0, 0, 0 };
            uint8_t other_wanted[5] = { other_setting->original[0], 9, 0, 0, 0 };
            memset(image + ADDR_Prepatch_Slots, 0, slots_size);
            image[setting_stub + 1] ^= 0x08;                                  // another register's mov
            memcpy(image + other_stub + 2, &far_away, sizeof(far_away));       // a slot that isn't among the slots
            image[third_stub] = 0x89;                                         // a mov to the slot, not from it
            image[call_stub + 9] = 0x07;                                      // not the je of a stub
            refused_before = refused;
            dobby_patches = 0;
            CHECK(BO3MacFix_CodePatch(image + setting->site, wanted, 5) == -1 && !prepatch_set_value(setting->site, 9),
                  "a setting with a damaged stub is set");
            CHECK(BO3MacFix_CodePatch(image + other_setting->site, other_wanted, 5) == -1 &&
                      !prepatch_set_value(other_setting->site, 9), "a setting whose stub reads no slot is set");
            CHECK(BO3MacFix_CodePatch(image + third_setting->site, third_wanted, 5) == -1 &&
                      !prepatch_set_value(third_setting->site, 9), "a setting whose stub doesn't load the slot is set");
            CHECK(!prepatch_redirect_call(call_defs[0].site, (void *)fake_function), "a call with a damaged stub is redirected");
            CHECK(refused == refused_before + 3 && dobby_patches == 0, "%i refused, %i patched in memory",
                  refused - refused_before, dobby_patches);
            CHECK(memcmp(image + ADDR_Prepatch_Slots, no_slots, slots_size) == 0, "a refused setting or call set a slot");
            // (the other call still works; a jump at a call's place isn't a call's stub, nor is a hooked function)
            CHECK(prepatch_redirect_call(call_defs[1].site, (void *)fake_function), "the call after the damaged one");
            image[call_defs[1].site] = 0xE9;
            CHECK(!prepatch_redirect_call(call_defs[1].site, (void *)fake_function), "a jump where the call was");
            CHECK(!prepatch_redirect_call(hook_defs[5].function, (void *)fake_function), "a hooked function as a call");
            free(no_slots);
            munmap(image, ADDR_Prepatch_SlotsEnd);
        }
        // A copy whose changes can't be known: one of another format, or one of this format made from other tables
        // (both say so in the header). The functions it hooked are still hooked through their stubs, because a stub
        // says everything about itself, and nothing is written over the copy's jumps. What it doesn't hook is hooked
        // in memory, and everything else is done as if there were no copy.
        for (int which = 0; which < 2; which++) {
            image = map_image(copy);
            if (image == NULL)
                continue;
            prepatch_header_t *its_header = (prepatch_header_t *)(image + ADDR_Prepatch_Header);
            if (which == 0)
                its_header->version = Prepatch_Version - 1;
            else
                its_header->table_id ^= 1;
            CHECK(!prepatch_init((uint64_t)image) && !prepatch_active() && foreign, "foreign copy %i counts as ours", which);
            dobby_hooks = dobby_patches = 0;
            uint32_t slot = ADDR_Prepatch_Slots;
            for (size_t i = 0; i < Hook_Count; i++, slot += 8) {
                void *orig = NULL;
                uint32_t stub = hook_defs[i].function + 5 + rel32(image + hook_defs[i].function + 1);
                CHECK(BO3MacFix_Hook(image + hook_defs[i].function, (void *)fake_function, &orig) == 0 &&
                          *(void **)(image + slot) == (void *)fake_function && orig == image + stub + 16,
                      "hook %08x of foreign copy %i", hook_defs[i].function, which);
            }
            CHECK(dobby_hooks == 0, "%i hooks of foreign copy %i were made in memory", dobby_hooks, which);
            void *orig = NULL;
            CHECK(BO3MacFix_Hook(image + 0x3000, (void *)fake_function, &orig) == 0 && dobby_hooks == 1,
                  "a hook such a copy doesn't have");
            CHECK(!prepatch_has_hook(hook_defs[0].function), "foreign copy %i has detours", which);
            // a call's stub has a hook's shape, but a call isn't a jump: it isn't taken for a hooked function
            CHECK(BO3MacFix_Hook(image + call_defs[0].site, (void *)fake_function, &orig) == 0 && dobby_hooks == 2,
                  "a redirected call of a foreign copy was hooked through its stub");
            // settings, calls and patches: nothing is known, so nothing is answered and patches are made in memory
            const uint8_t *hooked = image + hook_defs[0].function;
            CHECK(prepatch_original_code(hooked, 8) == hooked, "a foreign copy's original code");
            CHECK(!prepatch_set_value(value_defs[0].site, 2) && !prepatch_redirect_call(call_defs[0].site, (void *)fake_function),
                  "a setting or call of foreign copy %i was set through its slot", which);
            uint8_t nop[] = { 0x90 };
            CHECK(BO3MacFix_CodePatch(image + 0x3100, nop, 1) == 0 && dobby_patches == 1, "a patch with a foreign copy");
            // something that isn't the game's is hooked in memory as ever, also when it is as far behind the image's
            // start as a function the copy hooked
            CHECK(BO3MacFix_Hook((void *)fake_function, (void *)fake_function, &orig) == 0 && dobby_hooks == 3,
                  "a hook outside the game");
            CHECK(BO3MacFix_Hook(image + 0x100000000ull + hook_defs[1].function, (void *)fake_function, &orig) == 0 &&
                      dobby_hooks == 4, "a hook 4 GB behind a hooked function");
            munmap(image, ADDR_Prepatch_SlotsEnd);
            // and the next copy that is ours is read as such again
            image = map_image(copy);
            if (image != NULL) {
                dobby_hooks = dobby_patches = 0;
                CHECK(prepatch_init((uint64_t)image) && prepatch_active() && !foreign, "after foreign copy %i", which);
                munmap(image, ADDR_Prepatch_SlotsEnd);
            }
        }
    }

    // the room after the code is used up first: a stub only goes before the code when another one, 48 bytes at most
    // like the one before it, no longer fits there
    CHECK(stubs_before_code == 0 || last_stub_after_code + 2 * Stub_Max > first_area_end,
          "%i stubs before the code, with room after it (the last one there is at %08x of %08x)", stubs_before_code,
          last_stub_after_code, first_area_end);

    // With less room after the code, the stubs that don't fit go before it, and everything works the same. How much
    // less is taken from what this copy left free before the code, so that this holds for any number of hooks that
    // fit at all: up to 1 KB of stubs has to move there.
    uint32_t whole_first_area = first_area_end;
    uint32_t used_after = ADDR_Prepatch_CodeEnd - ADDR_Prepatch_Header;
    uint32_t used_before = ADDR_Prepatch_Stubs2End - ADDR_Prepatch_Stubs2;
    while (copy != NULL && used_after > 0 && copy[ADDR_Prepatch_Header + used_after - 1] == 0)
        used_after--;
    while (copy != NULL && used_before > 0 && copy[ADDR_Prepatch_Stubs2 + used_before - 1] == 0)
        used_before--;
    uint32_t free_before = ADDR_Prepatch_Stubs2End - ADDR_Prepatch_Stubs2 - used_before;
    // (the room is nearly used up when this fails: time to find more, before a hook more doesn't fit)
    CHECK(free_before >= 6 * Stub_Max, "only %u bytes are left for stubs", free_before);
    uint32_t moved = free_before >= 6 * Stub_Max ? free_before - 2 * Stub_Max : 0;
    if (moved > 0x400)
        moved = 0x400;
    int stubs_before_code_in_whole = stubs_before_code;
    first_area_end = (ADDR_Prepatch_Header + used_after - moved) & ~15u;
    CHECK(BO3MacFix_BuildPrepatched(original_path, other_path, message, sizeof(message)) == 0, "build: \"%s\"", message);
    printf("%s\n", message);
    size_t split_size = 0;
    uint8_t *split = read_file(other_path, &split_size);
    CHECK(split != NULL && split_size == File_Size, "the copy with stubs in both places is %zu bytes", split_size);
    if (split != NULL) {
        check_stubs(split);
        // (a stub is 48 bytes at most, so that many bytes less hold at least this many stubs less)
        CHECK(stubs_before_code >= stubs_before_code_in_whole + (int)(moved / Stub_Max) - 1 && stubs_before_code > 3,
              "%i stubs before the code, %i with all the room after it", stubs_before_code, stubs_before_code_in_whole);
        for (uint32_t o = first_area_end; o < whole_first_area; o++)
            CHECK(split[o] == 0, "a stub past the room it was given, at %08x", o);
        uint8_t *image = map_image(split);
        if (image != NULL) {
            dobby_hooks = dobby_patches = 0;
            check_runtime(image, original);
            munmap(image, ADDR_Prepatch_SlotsEnd);
        }
        free(split);
    }
    unlink(other_path);
    // no room for the header, and no room for all the stubs
    first_area_end = ADDR_Prepatch_Header + sizeof(prepatch_header_t) - 1;
    expect_failure("no room for the header", original_path, other_path, "out of room");
    first_area_end = ADDR_Prepatch_Header + sizeof(prepatch_header_t);
    uint32_t stubs_size = used_after - sizeof(prepatch_header_t) + used_before;
    if (stubs_size + Stub_Max > ADDR_Prepatch_Stubs2End - ADDR_Prepatch_Stubs2)
        expect_failure("no room for the stubs", original_path, other_path, "out of room");
    first_area_end = whole_first_area;

    // a game update: the copy no longer matches the original
    original[0x5000] ^= 1;
    write_file(original_path, original, File_Size);
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == -1 &&
              strstr(message, "updated") != NULL, "after an update: \"%s\"", message);
    original[0x5000] ^= 1;
    write_file(original_path, original, File_Size);
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == 0 &&
              strstr(message, "unchanged") == NULL, "the original back, hashed again: \"%s\"", message);

    // a damaged or short .checked file is hashed over
    size_t checked_size = 0;
    uint8_t *checked = read_file(checked_path, &checked_size);
    CHECK(checked != NULL && checked_size > 64, "the kept hash is %zu bytes", checked_size);
    if (checked != NULL) {
        write_file(checked_path, checked, 40);
        CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == 0 &&
                  strstr(message, "unchanged") == NULL, "a short .checked: \"%s\"", message);
        // a kept hash that isn't what the copy was made from: the copy isn't current
        checked[checked_size - 1] ^= 1;
        write_file(checked_path, checked, checked_size);
        CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == -1 &&
                  strstr(message, "updated") != NULL, "a .checked with another hash: \"%s\"", message);
        free(checked);
    }
    // the original replaced by another file with the same contents (a new inode): hashed again, and current
    write_file(other_path, original, File_Size);
    CHECK(rename(other_path, original_path) == 0, "couldn't replace the original");
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == 0 &&
              strstr(message, "unchanged") == NULL, "a replaced original: \"%s\"", message);
    CHECK(BO3MacFix_PrepatchedIsCurrent(original_path, copy_path, message, sizeof(message)) == 0 &&
              strstr(message, "unchanged") != NULL, "then from the kept hash: \"%s\"", message);

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
    original[ADDR_Prepatch_Stubs2End - 1] = 1;
    write_file(other_path, original, File_Size);
    expect_failure("padding before the code in use", other_path, copy_path, "isn't empty");
    original[ADDR_Prepatch_Stubs2End - 1] = 0;
    // an executable with more load commands, which reach into where the stubs would go
    ((struct mach_header_64 *)original)->sizeofcmds += ADDR_Prepatch_Stubs2;
    write_file(other_path, original, File_Size);
    expect_failure("longer load commands", other_path, copy_path, "doesn't have room");
    ((struct mach_header_64 *)original)->sizeofcmds -= ADDR_Prepatch_Stubs2;
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
    unlink(checked_path);
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
