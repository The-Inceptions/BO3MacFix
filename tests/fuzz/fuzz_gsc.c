// gsc_obj.c under the fuzzer (make fuzz): a compiled script or a patch script from a workshop mod's fastfile. What
// passes gsc_obj_check / gsic_parse is then used the way gsc_detours.c uses it in the game: every name, include,
// export and import read, the "#using" taken out of the include table and put back, the link check against a made-up
// game, and the detours added, resolved against the script itself and applied to its calls. A read or write outside
// the input is a fault (the input is a copy of exactly its size).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/gsc_obj.c"

// the made-up game: it has this script under any include name, and builtins with even name hashes
static const uint8_t *game_script_body;
static const uint8_t *game_script(void *context, const char *include) {
    (void)context;
    return include[0] != '\0' && strlen(include) % 3 != 0 ? game_script_body : NULL;
}
static bool game_builtin(void *context, uint32_t name, bool method, int *kind, int *min_params, int *max_params) {
    (void)context, (void)method;
    *kind = (int)(name % 3);
    *min_params = 0;
    *max_params = (int)(name >> 28);
    return (name & 1) == 0;
}

static void read_everything(const uint8_t *obj) {
    volatile size_t sink = strlen(gsc_obj_name(obj));
    for (unsigned i = 0; i < gsc_obj_include_count(obj); i++)
        sink += strlen(gsc_obj_include(obj, i));
    gsc_export_t e;
    for (unsigned i = 0; i < gsc_obj_export_count(obj); i++)
        gsc_obj_export(obj, i, &e);
    gsc_obj_find_export(obj, 1, 2, &e);
    gsc_import_t import;
    unsigned index;
    size_t cursor = 0;
    while (gsc_obj_next_import(obj, &index, &cursor, &import)) {
        for (unsigned c = 0; c < import.call_count; c++) {
            uint32_t call = gsc_import_call(&import, c);
            uint64_t slot;
            memcpy(&slot, obj + gsc_call_slot(call, import.flags), sizeof(slot));
            sink += (size_t)slot;
        }
    }
    (void)sink;
}

static void as_script(uint8_t *obj, size_t size) {
    if (!gsc_obj_check(obj, size))
        return;
    read_everything(obj);
    gsc_include_backup_t backup;
    uint8_t *before = malloc(size);
    memcpy(before, obj, size);
    gsc_includes_strip(obj, &backup);
    read_everything(obj);
    game_script_body = obj;
    char why[200];
    gsc_link_env_t env = { game_script, game_builtin, NULL, size & 1 };
    gsc_obj_links(obj, &env, why, sizeof(why));
    gsc_includes_restore(obj, &backup);
    if (memcmp(before, obj, size) != 0)
        abort(); // the include table must come back as it was
    free(before);
}

static void as_patch_script(uint8_t *asset, size_t size) {
    gsic_t gsic;
    if (!gsic_is(asset, size) || !gsic_parse(asset, size, &gsic))
        return;
    uint8_t *body = asset + (gsic.body - asset);
    as_script(body, gsic.body_size);
    static gsc_rules_t rules;
    gsc_rules_clear(&rules);
    for (uint32_t i = 0; i < gsic.count; i++) {
        gsic_detour_t d;
        gsic_detour(&gsic, i, &d);
        if (d.offset == 0 || d.offset > gsic.body_size || d.size > gsic.body_size - d.offset)
            abort(); // gsic_parse lets no detour's code reach outside the script
    }
    if (!gsc_rules_add(&rules, &gsic))
        return;
    // the patch script is loaded, and is the only script there: its detours are looked for in it
    const uint8_t *scripts[1] = { body };
    gsc_rules_resolve(&rules, scripts, 1);
    gsc_rules_patch(&rules, body);
    // the game patches every loaded script again when a new detour is found (after_load in gsc_detours.c)
    gsc_rules_patch(&rules, body);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // the harness's copy of the input can be written: the loader changes the include table and the call slots
    uint8_t *copy = malloc(size ? size : 1);
    memcpy(copy, data, size);
    as_patch_script(copy, size);
    memcpy(copy, data, size);
    as_script(copy, size);
    char out[256];
    char *text = malloc(size + 1);
    memcpy(text, data, size);
    text[size] = '\0';
    gsc_mirror_name(text, size & 1 ? "zm_theater" : NULL, out, sizeof(out));
    gsc_map_name_valid(text);
    gsc_hash(text);
    free(text);
    free(copy);
    return 0;
}

// A made-up compiled script: a name, one include, two exports, one import with two calls; and a patch script around it
// with one detour of its second export's code.
static size_t make_script(uint8_t *obj) {
    memset(obj, 0, 0x200);
    uint64_t magic = Gsc_Magic;
    memcpy(obj, &magic, sizeof(magic));
    uint32_t name = 0x100, include_table = 0x120, include_text = 0x130, code = 0x48, code_size = 0x60,
             exports = 0x140, imports = 0x170;
    memcpy(obj + 0x100, "scripts/zm/_zm.gsc", 19);
    memcpy(obj + include_text, "#using", 7);
    memcpy(obj + 0x138, "scripts/shared/util", 20);
    uint32_t includes[2] = { include_text, 0x138 };
    memcpy(obj + include_table, includes, sizeof(includes));
    memcpy(obj + Gsc_Name_Offset, &name, 4);
    memcpy(obj + Gsc_Include_Offset, &include_table, 4);
    obj[Gsc_Include_Count] = 2;
    memcpy(obj + Gsc_Code_Offset, &code, 4);
    memcpy(obj + Gsc_Code_Size, &code_size, 4);
    memcpy(obj + Gsc_Exports_Offset, &exports, 4);
    uint16_t export_count = 2, import_count = 1;
    memcpy(obj + Gsc_Exports_Count, &export_count, 2);
    uint32_t e1[5] = { 0, code, 0x1111, 0x2222, 0 }, e2[5] = { 0, code + 0x20, 0x3333, 0x2222, 0 };
    memcpy(obj + exports, e1, 20);
    memcpy(obj + exports + 20, e2, 20);
    memcpy(obj + Gsc_Imports_Offset, &imports, 4);
    memcpy(obj + Gsc_Imports_Count, &import_count, 2);
    uint32_t import[3] = { 0x1111, 0x2222, 2 | (0 << 16) };
    memcpy(obj + imports, import, 12);
    obj[imports + 11] = Gsc_Call_Function;
    uint32_t calls[2] = { code + 0x8, code + 0x28 };
    memcpy(obj + imports + 12, calls, 8);
    // the function's address behind each call, as the linker leaves it
    uint64_t linked = 0x1000 + code;
    memcpy(obj + gsc_call_slot(calls[0], Gsc_Call_Function), &linked, 8);
    memcpy(obj + gsc_call_slot(calls[1], Gsc_Call_Function), &linked, 8);
    return 0x200;
}
void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    static uint8_t asset[0x400];
    size_t size = make_script(asset);
    add(asset, size);
    memset(asset, 0, sizeof(asset));
    uint32_t header[4] = { Gsic_Magic, 1, 0, 1 };
    memcpy(asset, header, sizeof(header));
    uint32_t detour[5] = { 0x4444, 0x2222, 0x1111, 0x48 + 0x20, 0x20 };
    memcpy(asset + 16, detour, sizeof(detour));
    memcpy(asset + 16 + Gsic_Path_Offset, "scripts/zm/_zm.gsc", 19);
    size = make_script(asset + 16 + Gsic_Entry_Size);
    add(asset, 16 + Gsic_Entry_Size + size);
}
void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    add("\x80GSC\r\n\0\x1c", 8);
    add("GSIC", 4);
    add("#using", 7);
    add("scripts/zm/", 11);
    add("scripts/aae_common/", 19);
    add(".gsc", 4);
}
