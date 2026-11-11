// gsc_obj.c against made-up compiled scripts and patch scripts, and the installed All-around Enhancement's
// core_mod.ff if there is one. Built with the address and undefined behaviour sanitizers, since it parses files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../source/gsc_obj.h"
#include "fake_gsc.h"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#pragma mark - tests

static void test_hash(void) {
    CHECK(gsc_hash("zm") == 0xac8e5d72);
    CHECK(gsc_hash("round_think") == 0xe643ed8e);
    CHECK(gsc_hash("Round_Think") == 0xe643ed8e);
    CHECK(gsc_hash("main") == 0xd290ebfa);
    CHECK(gsc_hash("") == 0x33b293fd);
    CHECK(gsc_hash("sys") == 0x2e3ca4c4);
}

static void test_check(void) {
    static uint8_t script[0x2000];
    const char *includes[] = { "scripts/zm/_zm", "scripts/shared/util_shared" };
    made_export_t exports[] = { { 0x11, 0x22, Code_At + 8, 1, 0 } };
    made_import_t imports[] = { { 0x33, 0x44, 2, Gsc_Call_Function, { Code_At + 0x20, Code_At + 0x3f0 }, 2 } };
    size_t size = make_script(script, "scripts/zm/test.gsc", 0, includes, 2, exports, 1, imports, 1);
    CHECK(gsc_obj_check(script, size));
    CHECK(strcmp(gsc_obj_name(script), "scripts/zm/test.gsc") == 0);
    CHECK(gsc_obj_include_count(script) == 2 && strcmp(gsc_obj_include(script, 1), "scripts/shared/util_shared") == 0);
    gsc_export_t e;
    CHECK(gsc_obj_find_export(script, 0x11, 0x22, &e) && e.address == Code_At + 8 && e.params == 1);
    CHECK(!gsc_obj_find_export(script, 0x22, 0x11, &e));
    CHECK(gsc_call_slot(0x314, Gsc_Call_Function) == 0x318 && gsc_call_slot(0x316, Gsc_Call_Function) == 0x320);
    CHECK(gsc_call_slot(0x6b8, Gsc_Call_Address) == 0x6c0 && gsc_call_slot(0x6b6, Gsc_Call_Address) == 0x6b8);

    // every shorter size is refused, read from a block of exactly that size so the sanitizer sees any overrun
    for (size_t cut = 0; cut < size; cut++) {
        uint8_t *exact = malloc(cut + 1);
        memcpy(exact, script, cut);
        CHECK(!gsc_obj_check(exact, cut));
        free(exact);
    }
    // tables and texts that point outside
    static uint8_t bad[0x2000];
    uint32_t fields[] = { Gsc_Name_Offset, Gsc_Include_Offset, Gsc_Exports_Offset, Gsc_Imports_Offset, Gsc_Code_Offset,
                          Gsc_Code_Size };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        memcpy(bad, script, size);
        put32(bad + fields[i], 0xfffffff0);
        CHECK(!gsc_obj_check(bad, size));
        memcpy(bad, script, size);
        put32(bad + fields[i], (uint32_t)size);
        CHECK(!gsc_obj_check(bad, size) || fields[i] == Gsc_Code_Size);
    }
    memcpy(bad, script, size);
    put16(bad + Gsc_Imports_Count, 0xffff);
    CHECK(!gsc_obj_check(bad, size));
    memcpy(bad, script, size);
    put16(bad + Gsc_Exports_Count, 0xffff);
    CHECK(!gsc_obj_check(bad, size));
    memcpy(bad, script, size);
    bad[Gsc_Include_Count] = 0xff;
    CHECK(!gsc_obj_check(bad, size));
    memcpy(bad, script, size);
    bad[0] ^= 1;
    CHECK(!gsc_obj_check(bad, size));
    // a call at the very end, whose address would lie outside
    memcpy(bad, script, size);
    put32(bad + size - 4, (uint32_t)size - 4);
    CHECK(!gsc_obj_check(bad, size));
    CHECK(!gsc_obj_check(NULL, 100));
}

static void test_includes(void) {
    static uint8_t script[0x2000], before[0x2000];
    const char *includes[] = { "#using", "scripts/zm/_zm", "#USING_ANIMTREE", "scripts/zm/_zm_audio", "#include" };
    size_t size = make_script(script, "", 0, includes, 5, NULL, 0, NULL, 0);
    memcpy(before, script, size);
    CHECK(gsc_include_is_directive("#Using") && !gsc_include_is_directive("#usin") &&
          !gsc_include_is_directive("scripts/#using") && !gsc_include_is_directive("#using "));
    gsc_include_backup_t backup;
    CHECK(gsc_includes_strip(script, &backup));
    CHECK(gsc_obj_include_count(script) == 2);
    CHECK(strcmp(gsc_obj_include(script, 0), "scripts/zm/_zm") == 0);
    CHECK(strcmp(gsc_obj_include(script, 1), "scripts/zm/_zm_audio") == 0);
    CHECK(gsc_obj_check(script, size));
    gsc_includes_restore(script, &backup);
    CHECK(memcmp(script, before, size) == 0);
    // nothing to take out: nothing changes, either way
    const char *clean[] = { "scripts/zm/_zm" };
    size = make_script(script, "", 0, clean, 1, NULL, 0, NULL, 0);
    memcpy(before, script, size);
    CHECK(!gsc_includes_strip(script, &backup));
    CHECK(memcmp(script, before, size) == 0);
    gsc_includes_restore(script, &backup);
    CHECK(memcmp(script, before, size) == 0);
}

// the game for gsc_obj_links: one included script, and three builtins
static uint8_t library[0x2000];
static const uint8_t *game_script(void *context, const char *include) {
    (void)context;
    return strcmp(include, "scripts/zm/library") == 0 ? library : NULL;
}
static bool game_builtin(void *context, uint32_t name, bool method, int *kind, int *min_params, int *max_params) {
    (void)context;
    static const struct {
        uint32_t name;
        bool method;
        int kind, min_params, max_params;
    } builtins[] = { { 0x100, false, 0, 1, 2 }, { 0x101, true, 0, 0, 1 }, { 0x102, false, 1, 0, 0 },
                     { 0x103, false, 2, 0, 0 } };
    for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (builtins[i].name == name && builtins[i].method == method) {
            *kind = builtins[i].kind;
            *min_params = builtins[i].min_params;
            *max_params = builtins[i].max_params;
            return true;
        }
    }
    return false;
}

static bool links(uint8_t script_flags, const made_import_t *import, const char *include, bool skips_dev) {
    static uint8_t script[0x2000];
    made_export_t own[] = { { 0xaa, 0x1, Code_At + 8, 1, 0 } };
    const char *includes[] = { include };
    size_t size = make_script(script, "", script_flags, includes, include != NULL ? 1 : 0, own, 1, import, 1);
    CHECK(gsc_obj_check(script, size));
    gsc_link_env_t env = { game_script, game_builtin, NULL, skips_dev };
    char why[160] = "";
    bool ok = gsc_obj_links(script, &env, why, sizeof(why));
    CHECK(ok == (why[0] == '\0'));
    return ok;
}

static void test_links(void) {
    made_export_t exports[] = { { 0xbb, 0x2, Code_At + 8, 2, 0 }, { 0xbb, 0x3, Code_At + 0x40, 0, Gsc_Export_Private },
                                { 0xbb, 0x4, Code_At + 0x80, 0, Gsc_Export_Varargs } };
    make_script(library, "scripts/zm/library.gsc", 0, NULL, 0, exports, 3, NULL, 0);
    const char *lib = "scripts/zm/library";
    uint32_t none = gsc_hash(""), sys = gsc_hash("sys");
#define IMPORT(ns, name, params, flags) &(made_import_t){ ns, name, params, flags, { Code_At + 0x10 }, 1 }
    // its own functions, and an include's public ones
    CHECK(links(0, IMPORT(0xaa, 0x1, 1, Gsc_Call_Function), lib, false));
    CHECK(links(0, IMPORT(0xaa, 0x1, 0, Gsc_Call_Thread), lib, false));
    CHECK(!links(0, IMPORT(0xaa, 0x1, 2, Gsc_Call_Function), lib, false)); // more parameters than it has
    CHECK(links(0, IMPORT(0xaa, 0x1, 2, Gsc_Call_Address), lib, false));   // &f passes none
    CHECK(links(0, IMPORT(0xbb, 0x2, 2, Gsc_Call_Method), lib, false));
    CHECK(links(0, IMPORT(0xbb, 0x2, 1, Gsc_Call_Method_Thread), lib, false));
    CHECK(!links(0, IMPORT(0xbb, 0x2, 3, Gsc_Call_Method), lib, false));
    CHECK(!links(0, IMPORT(0xbb, 0x3, 0, Gsc_Call_Function), lib, false)); // private
    CHECK(links(0, IMPORT(0xbb, 0x4, 9, Gsc_Call_Function), lib, false));  // any number of parameters
    CHECK(!links(0, IMPORT(0xbb, 0x2, 2, Gsc_Call_Function), NULL, false)); // not included
    CHECK(!links(0, IMPORT(0xbb, 0x5, 0, Gsc_Call_Function), lib, false));
    CHECK(!links(0, IMPORT(0xbb, 0x2, 2, 0), lib, false)); // no such kind of call
    CHECK(!links(0, IMPORT(0xbb, 0x2, 2, 6), lib, false));
    // an include the game doesn't have
    CHECK(!links(0, IMPORT(0xaa, 0x1, 1, Gsc_Call_Function), "scripts/zm/missing", false));
    CHECK(!links(0, IMPORT(0xaa, 0x1, 1, Gsc_Call_Function), "#using", false));
    // builtins: only without a namespace or with the builtins' own, with a parameter count they take
    CHECK(links(0, IMPORT(0xaa, 0x100, 1, Gsc_Call_Function | Gsc_Import_Any), lib, false));
    CHECK(links(0, IMPORT(none, 0x100, 2, Gsc_Call_Function), lib, false));
    CHECK(links(0, IMPORT(sys, 0x100, 2, Gsc_Call_Function), lib, false));
    CHECK(!links(0, IMPORT(0xaa, 0x100, 1, Gsc_Call_Function), lib, false));
    CHECK(!links(0, IMPORT(none, 0x100, 0, Gsc_Call_Function), lib, false));
    CHECK(!links(0, IMPORT(none, 0x100, 3, Gsc_Call_Function), lib, false));
    CHECK(!links(0, IMPORT(none, 0x100, 1, Gsc_Call_Method), lib, false)); // a function, not a method
    CHECK(!links(0, IMPORT(none, 0x100, 1, Gsc_Call_Thread), lib, false)); // no thread on a builtin
    CHECK(links(0, IMPORT(none, 0x101, 1, Gsc_Call_Method), lib, false));
    CHECK(!links(0, IMPORT(none, 0x101, 1, Gsc_Call_Function), lib, false));
    CHECK(!links(0, IMPORT(none, 0x101, 1, Gsc_Call_Method_Thread), lib, false));
    CHECK(links(0, IMPORT(none, 0x100, 7, Gsc_Call_Address), lib, false)); // &builtin, function or method
    CHECK(links(0, IMPORT(none, 0x101, 7, Gsc_Call_Address), lib, false));
    CHECK(!links(0, IMPORT(none, 0x104, 0, Gsc_Call_Address), lib, false));
    // dev-only builtins need a /# #/ call, and builtins of kind 2 an unrestricted script
    CHECK(!links(0, IMPORT(none, 0x102, 0, Gsc_Call_Function), lib, false));
    CHECK(links(0, IMPORT(none, 0x102, 0, Gsc_Call_Function | Gsc_Import_Dev), lib, false));
    CHECK(links(0, IMPORT(none, 0x103, 0, Gsc_Call_Function), lib, false));
    CHECK(!links(Gsc_Flag_Restricted, IMPORT(none, 0x103, 0, Gsc_Call_Function), lib, false));
    // /# #/ calls of an unrestricted script aren't linked at all when the game leaves them out
    CHECK(links(0, IMPORT(0xbb, 0x5, 0, Gsc_Call_Function | Gsc_Import_Dev), lib, true));
    CHECK(!links(0, IMPORT(0xbb, 0x5, 0, Gsc_Call_Function | Gsc_Import_Dev), lib, false));
    CHECK(!links(Gsc_Flag_Restricted, IMPORT(0xbb, 0x5, 0, Gsc_Call_Function | Gsc_Import_Dev), lib, true));
#undef IMPORT
}

static void test_gsic(void) {
    static uint8_t body[0x2000], asset[0x4000], bad[0x4000];
    made_export_t exports[] = { { 0xcc, 0x9, Code_At + 0x10, 0, 0 } };
    size_t body_size = make_script(body, "", 0, NULL, 0, exports, 1, NULL, 0);
    gsic_detour_t detours[] = { { 0x9, 0xac8e5d72, 0xe643ed8e, Code_At + 0x10, 0x40, "scripts/zm/_zm.gsc" },
                                { 0x9, 0xac8e5d72, 0x1, Code_At + 0x60, Code_Size - 0x60, "Scripts\\ZM\\_zm.gsc" } };
    size_t size = make_gsic(asset, detours, 2, body, body_size);
    gsic_t gsic;
    CHECK(gsic_is(asset, size) && !gsic_is(body, body_size) && !gsic_is(asset, 4));
    CHECK(gsic_parse(asset, size, &gsic));
    CHECK(gsic.count == 2 && gsic.body == asset + 16 + 2 * Gsic_Entry_Size && gsic.body_size == body_size);
    gsic_detour_t d;
    gsic_detour(&gsic, 1, &d);
    CHECK(d.target_name == 0x1 && d.offset == Code_At + 0x60 && strcmp(d.target_script, "Scripts\\ZM\\_zm.gsc") == 0);
    // every shorter size is refused
    for (size_t cut = 0; cut < size; cut++) {
        uint8_t *exact = malloc(cut + 1);
        memcpy(exact, asset, cut);
        CHECK(!gsic_parse(exact, cut, &gsic));
        free(exact);
    }
#define BAD(change)                                                                                                    \
    do {                                                                                                               \
        memcpy(bad, asset, size);                                                                                      \
        change;                                                                                                        \
        CHECK(!gsic_parse(bad, size, &gsic));                                                                          \
    } while (0)
    uint8_t *first = bad + 16;
    BAD(put32(bad + 4, 9));                    // too many sections
    BAD(put32(bad + 8, 1));                    // the client's scripts
    BAD(put32(bad + 12, 0x401));               // too many detours
    BAD(put32(bad + 12, 3));                   // more than there are
    BAD(first[Gsic_Path_Offset] = '\0');       // no target script
    BAD(memset(first + Gsic_Path_Offset, 'a', Gsic_Entry_Size - Gsic_Path_Offset)); // a name that never ends
    BAD(memset(first + Gsic_Path_Offset, 'a', Gsic_Max_Path + 1));                  // or is too long
    BAD(put32(first + 4, 0));                  // no namespace
    BAD(put32(first + 8, 0));                  // no function
    BAD(put32(first + 12, 0));                 // no code
    BAD(put32(first + 16, 0));
    BAD(put32(first + 12, (uint32_t)body_size)); // code outside the script
    BAD(put32(first + 16, (uint32_t)body_size));
    BAD(bad[16 + 2 * Gsic_Entry_Size] ^= 1);   // not a script behind the header
#undef BAD
    // no detours at all is a sound patch script with nothing to do
    size = make_gsic(asset, NULL, 0, body, body_size);
    CHECK(gsic_parse(asset, size, &gsic) && gsic.count == 0);
}

static void test_names(void) {
    char name[128];
    CHECK(gsc_mirror_name("scripts/zm/_zm.gsc", NULL, name, sizeof(name)));
    CHECK(strcmp(name, "scripts/aae_common/zm/_zm") == 0);
    CHECK(gsc_mirror_name("Scripts\\ZM\\zm_moon.GSC", "zm_moon", name, sizeof(name)));
    CHECK(strcmp(name, "scripts/aae_level/zm_moon/zm/zm_moon") == 0);
    CHECK(gsc_mirror_name("scripts/shared/util_shared.gsc", NULL, name, sizeof(name)));
    CHECK(strcmp(name, "scripts/aae_common/shared/util_shared") == 0);
    CHECK(!gsc_mirror_name("scripts/zm/_zm.csc", NULL, name, sizeof(name)));
    CHECK(!gsc_mirror_name("scripts/zm/_zm", NULL, name, sizeof(name)));
    CHECK(!gsc_mirror_name("", NULL, name, sizeof(name)));
    CHECK(!gsc_mirror_name("maps/zm/_zm.gsc", NULL, name, sizeof(name)));
    CHECK(!gsc_mirror_name("scripts/.gsc", NULL, name, sizeof(name)));
    CHECK(!gsc_mirror_name("scripts/aae_common/zm/_zm.gsc", NULL, name, sizeof(name)));
    CHECK(!gsc_mirror_name("scripts/aae_level/zm_moon/main.gsc", "zm_moon", name, sizeof(name)));
    CHECK(!gsc_mirror_name("scripts/zm/_zm.gsc", NULL, name, 20)); // doesn't fit
    CHECK(gsc_map_name_valid("zm_factory") && gsc_map_name_valid("zm_x") && gsc_map_name_valid("zm_"));
    CHECK(!gsc_map_name_valid("zm") && !gsc_map_name_valid("mp_nuketown") && !gsc_map_name_valid("ZM_FACTORY"));
    CHECK(!gsc_map_name_valid("zm_../x") && !gsc_map_name_valid("zm_a b") && !gsc_map_name_valid(""));
    CHECK(!gsc_map_name_valid("zm_3456789012345678901234567890123456789012345678901234567890abcd"));
}

static void test_rules(void) {
    static uint8_t zm[0x2000], other[0x2000], mirror_body[0x2000], asset[0x4000], second_asset[0x4000];
    // scripts/zm/_zm.gsc: two functions, and it calls both itself
    made_export_t zm_exports[] = { { 0xac, 0x1, Code_At + 0x10, 1, 0 }, { 0xac, 0x2, Code_At + 0x100, 0, 0 } };
    made_import_t zm_imports[] = { { 0xac, 0x1, 1, Gsc_Call_Function, { Code_At + 0x120 }, 1 },
                                   { 0xac, 0x2, 0, Gsc_Call_Thread, { Code_At + 0x140, Code_At + 0x16a }, 2 } };
    make_script(zm, "scripts/zm/_zm.gsc", Gsc_Flag_Restricted, NULL, 0, zm_exports, 2, zm_imports, 2);
    // another script: calls the first, takes the address of the second, and calls a builtin
    made_import_t other_imports[] = { { 0xac, 0x1, 1, Gsc_Call_Method, { Code_At + 0x20 }, 1 },
                                      { 0xac, 0x2, 0, Gsc_Call_Address, { Code_At + 0x46 }, 1 },
                                      { 0xdd, 0x100, 0, Gsc_Call_Function | Gsc_Import_Any, { Code_At + 0x60 }, 1 } };
    make_script(other, "scripts/zm/other.gsc", 0, NULL, 0, NULL, 0, other_imports, 3);
    // the patch script: replacements of both, the first calling the original and the second function
    made_export_t mirror_exports[] = { { 0xee, 0x11, Code_At + 0x10, 1, 0 }, { 0xee, 0x12, Code_At + 0x200, 0, 0 } };
    made_import_t mirror_imports[] = { { 0xac, 0x1, 1, Gsc_Call_Function, { Code_At + 0x20, Code_At + 0x210 }, 2 },
                                       { 0xac, 0x2, 0, Gsc_Call_Function, { Code_At + 0x40, Code_At + 0x220 }, 2 } };
    size_t body_size = make_script(mirror_body, "", 0, NULL, 0, mirror_exports, 2, mirror_imports, 2);
    gsic_detour_t detours[] = { { 0x11, 0xac, 0x1, Code_At + 0x10, 0x80, "scripts/zm/_zm.gsc" },
                                { 0x12, 0xac, 0x2, Code_At + 0x200, 0x80, "Scripts\\zm\\_ZM.gsc" },
                                { 0x12, 0xac, 0x3, Code_At + 0x200, 0x80, "scripts/zm/_zm.gsc" },       // no such function
                                { 0x12, 0xac, 0x2, Code_At + 0x200, 0x80, "scripts/zm/later.gsc" } }; // not loaded
    size_t size = make_gsic(asset, detours, 4, mirror_body, body_size);
    gsic_t gsic;
    CHECK(gsic_parse(asset, size, &gsic));
    uint8_t *mirror = asset + (gsic.body - asset);
    const uint8_t *f1 = zm + Code_At + 0x10, *f2 = zm + Code_At + 0x100, *builtin = (const uint8_t *)0x1234;
    const uint8_t *r1 = mirror + Code_At + 0x10, *r2 = mirror + Code_At + 0x200;
    link_import(zm, 0, f1), link_import(zm, 1, f2);
    link_import(other, 0, f1), link_import(other, 1, f2), link_import(other, 2, builtin);
    link_import(mirror, 0, f1), link_import(mirror, 1, f2);

    static gsc_rules_t rules;
    gsc_rules_clear(&rules);
    CHECK(gsc_rules_patch(&rules, zm) == 0);
    CHECK(gsc_rules_add(&rules, &gsic) && rules.count == 4);
    const uint8_t *loaded[] = { other, zm, mirror };
    CHECK(gsc_rules_resolve(&rules, loaded, 1) == 0 && rules.resolved == 0); // its script isn't loaded yet
    CHECK(gsc_rules_patch(&rules, other) == 0);
    CHECK(gsc_rules_resolve(&rules, loaded, 3) == 2 && rules.resolved == 2);
    CHECK(rules.rules[0].target == f1 && rules.rules[1].target == f2 && rules.rules[2].target == NULL);
    CHECK(gsc_rules_resolve(&rules, loaded, 3) == 0);

    CHECK(gsc_rules_patch(&rules, zm) == 3);
    CHECK(get64(zm + gsc_call_slot(Code_At + 0x120, Gsc_Call_Function)) == (uint64_t)r1);
    CHECK(get64(zm + gsc_call_slot(Code_At + 0x140, Gsc_Call_Thread)) == (uint64_t)r2);
    CHECK(get64(zm + gsc_call_slot(Code_At + 0x16a, Gsc_Call_Thread)) == (uint64_t)r2);
    CHECK(gsc_rules_patch(&rules, other) == 2);
    CHECK(get64(other + gsc_call_slot(Code_At + 0x20, Gsc_Call_Method)) == (uint64_t)r1);
    CHECK(get64(other + gsc_call_slot(Code_At + 0x46, Gsc_Call_Address)) == (uint64_t)r2);
    CHECK(get64(other + gsc_call_slot(Code_At + 0x60, Gsc_Call_Function)) == (uint64_t)builtin);
    // in the patch script: a replacement's own calls of what it replaces stay, the rest go to the replacements
    CHECK(gsc_rules_patch(&rules, mirror) == 2);
    CHECK(get64(mirror + gsc_call_slot(Code_At + 0x20, Gsc_Call_Function)) == (uint64_t)f1);  // in r1, calls f1
    CHECK(get64(mirror + gsc_call_slot(Code_At + 0x210, Gsc_Call_Function)) == (uint64_t)r1); // in r2, calls f1
    CHECK(get64(mirror + gsc_call_slot(Code_At + 0x40, Gsc_Call_Function)) == (uint64_t)r2);  // in r1, calls f2
    CHECK(get64(mirror + gsc_call_slot(Code_At + 0x220, Gsc_Call_Function)) == (uint64_t)f2); // in r2, calls f2
    // a second pass finds nothing left
    CHECK(gsc_rules_patch(&rules, zm) == 0 && gsc_rules_patch(&rules, other) == 0);
    CHECK(gsc_rules_patch(&rules, mirror) == 0);

    // a second patch script replacing the first function too: the first one keeps it, but calls of the function
    // inside the second one's replacement stay as well
    link_import(zm, 0, f1), link_import(other, 0, f1);
    made_import_t second_imports[] = { { 0xac, 0x1, 1, Gsc_Call_Function, { Code_At + 0x20, Code_At + 0x300 }, 2 } };
    body_size = make_script(mirror_body, "", 0, NULL, 0, mirror_exports, 1, second_imports, 1);
    gsic_detour_t again[] = { { 0x11, 0xac, 0x1, Code_At + 0x10, 0x80, "scripts/zm/_zm.gsc" } };
    size = make_gsic(second_asset, again, 1, mirror_body, body_size);
    gsic_t second_gsic;
    CHECK(gsic_parse(second_asset, size, &second_gsic));
    uint8_t *second = second_asset + (second_gsic.body - second_asset);
    link_import(second, 0, f1);
    CHECK(gsc_rules_add(&rules, &second_gsic) && rules.count == 5);
    const uint8_t *more[] = { other, zm, mirror, second };
    CHECK(gsc_rules_resolve(&rules, more, 4) == 1 && rules.resolved == 3);
    CHECK(gsc_rules_patch(&rules, zm) == 1 && gsc_rules_patch(&rules, other) == 1);
    CHECK(get64(zm + gsc_call_slot(Code_At + 0x120, Gsc_Call_Function)) == (uint64_t)r1);
    CHECK(gsc_rules_patch(&rules, second) == 1);
    CHECK(get64(second + gsc_call_slot(Code_At + 0x20, Gsc_Call_Function)) == (uint64_t)f1);
    CHECK(get64(second + gsc_call_slot(Code_At + 0x300, Gsc_Call_Function)) == (uint64_t)r1);

    // more detours than fit: none are added
    gsc_rules_clear(&rules);
    rules.count = Gsic_Max_Detours - 3;
    CHECK(!gsc_rules_add(&rules, &gsic) && rules.count == Gsic_Max_Detours - 3);
    rules.count = Gsic_Max_Detours - 4;
    CHECK(gsc_rules_add(&rules, &gsic) && rules.count == Gsic_Max_Detours);
}

#pragma mark - the installed All-around Enhancement

// decompresses a fastfile's blocks (the same walk as aae_build.c), returns the data or NULL
static uint8_t *read_fastfile(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)file_size);
    if (fread(file, 1, (size_t)file_size, f) != (size_t)file_size || file_size < 0x248 || memcmp(file, "TAff", 4) != 0) {
        fclose(f);
        free(file);
        return NULL;
    }
    fclose(f);
    size_t capacity = (size_t)file_size * 4, used = 0, size = (size_t)file_size, pos = 0x248;
    uint8_t *out = malloc(capacity);
    while (pos + 16 <= size) {
        uint32_t h[4];
        memcpy(h, file + pos, 16);
        bool block = h[3] == pos && h[0] > 0 && h[0] <= h[2] && h[2] - h[0] < 64 && h[2] <= (16u << 20) &&
                     h[1] <= (16u << 20) && pos + 16 + h[2] <= size;
        if (!block) {
            pos = ((pos + 3) & ~(size_t)3) + (pos % 4 == 0 ? 4 : 0);
            continue;
        }
        if (used + h[1] > capacity) {
            capacity = (used + h[1]) * 2;
            out = realloc(out, capacity);
        }
        // a zlib stream, a bare deflate stream, or the bytes as they are
        uLongf length = h[1];
        if (uncompress(out + used, &length, file + pos + 16, h[0]) != Z_OK) {
            z_stream z = { 0 };
            z.next_in = file + pos + 16;
            z.avail_in = h[0];
            z.next_out = out + used;
            z.avail_out = h[1];
            length = 0;
            if (inflateInit2(&z, -15) == Z_OK) {
                if (inflate(&z, Z_FINISH) == Z_STREAM_END)
                    length = z.total_out;
                inflateEnd(&z);
            }
            if (length == 0) {
                length = h[1] < h[0] ? h[1] : h[0];
                memcpy(out + used, file + pos + 16, length);
            }
        }
        used += length;
        pos += 16 + h[2];
    }
    free(file);
    *out_size = used;
    return out;
}

static void test_installed_aae(void) {
    char path[1024];
    snprintf(path, sizeof(path),
             "%s/Library/Application Support/Steam/steamapps/workshop/content/311210/2631943123/core_mod.ff",
             getenv("HOME") != NULL ? getenv("HOME") : "");
    size_t size = 0;
    uint8_t *data = read_fastfile(path, &size);
    if (data == NULL) {
        printf("no All-around Enhancement installed, skipping the real core_mod.ff\n");
        return;
    }
    unsigned scripts = 0, detours = 0, stripped = 0, bad = 0;
    static gsc_rules_t rules;
    gsc_rules_clear(&rules);
    for (size_t at = 0; at + 16 < size; at++) {
        if (memcmp(data + at, "GSIC", 4) != 0 || at < 2 || data[at - 1] != '\0')
            continue;
        // the asset's name sits right before it, after the asset's own fields (which end in 0xff bytes)
        size_t name_at = at - 1;
        while (name_at > 0 && data[name_at - 1] >= 0x20 && data[name_at - 1] < 0x7f && at - name_at < 200)
            name_at--;
        const char *asset_name = (const char *)data + name_at;
        if (strncmp(asset_name, "scripts/aae_", 12) != 0)
            continue;
        // its size is in the asset before the name; whatever follows in the fastfile doesn't change the answer
        gsic_t gsic;
        size_t length = size - at < 0x100000 ? size - at : 0x100000;
        if (!gsic_parse(data + at, length, &gsic)) {
            printf("FAIL: %s doesn't parse\n", asset_name);
            bad++;
            continue;
        }
        scripts++;
        detours += gsic.count;
        uint8_t *body = data + (gsic.body - data);
        for (uint32_t i = 0; i < gsic.count; i++) {
            // each replacement is a function the script exports, and the detour names the script it is the patch of
            gsic_detour_t d;
            gsic_detour(&gsic, i, &d);
            gsc_export_t e;
            bool found = false;
            for (unsigned j = 0; j < gsc_obj_export_count(body) && !found; j++) {
                gsc_obj_export(body, j, &e);
                found = e.name == d.replacement && e.address == d.offset;
            }
            char common[256], level[256], expected[256];
            snprintf(expected, sizeof(expected), "%.*s", (int)(strlen(asset_name) - 4), asset_name);
            const char *map = strncmp(asset_name, "scripts/aae_level/", 18) == 0 ? asset_name + 18 : NULL;
            char map_name[64] = "";
            if (map != NULL)
                snprintf(map_name, sizeof(map_name), "%.*s", (int)strcspn(map, "/"), map);
            bool named = gsc_mirror_name(d.target_script, NULL, common, sizeof(common)) &&
                         (map == NULL ? strcmp(common, expected) == 0
                                      : gsc_map_name_valid(map_name) &&
                                            gsc_mirror_name(d.target_script, map_name, level, sizeof(level)));
            if (!found || !named) {
                printf("FAIL: %s detour %u (%s): %s\n", asset_name, i, d.target_script,
                       !found ? "no such export" : "not this script's patch");
                bad++;
            }
        }
        gsc_include_backup_t backup;
        if (gsc_includes_strip(body, &backup)) {
            stripped++;
            for (unsigned i = 0; i < gsc_obj_include_count(body); i++)
                if (strncmp(gsc_obj_include(body, i), "scripts/", 8) != 0)
                    bad++;
            gsc_includes_restore(body, &backup);
        }
        if (!gsc_rules_add(&rules, &gsic))
            bad++;
    }
    CHECK(bad == 0);
    printf("installed AAE's core_mod.ff: %u patch scripts with %u detours, %u with include entries to take out\n",
           scripts, detours, stripped);
    free(data);
}

int main(void) {
    test_hash();
    test_check();
    test_includes();
    test_links();
    test_gsic();
    test_names();
    test_rules();
    test_installed_aae();
    if (failures != 0) {
        printf("test_gsc_obj: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_gsc_obj: all passed\n");
    return 0;
}
