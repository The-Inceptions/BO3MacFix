// Made-up compiled scripts and patch scripts for the tests of gsc_obj.c and gsc_detours.c.
#pragma once
#include <stdint.h>
#include <string.h>
#include "../source/gsc_obj.h"

typedef struct {
    uint32_t name_space, name, address;
    uint8_t params, flags;
} made_export_t;
typedef struct {
    uint32_t name_space, name;
    uint8_t params, flags;
    uint32_t calls[4];
    unsigned call_count;
} made_import_t;

#define Code_At 0x100
#define Code_Size 0x400

static inline void put16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}
static inline void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}
static inline uint64_t get64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

// a compiled script: header, name, code (zeros), includes, exports, imports; returns its size
static inline size_t make_script(uint8_t *out, const char *name, uint8_t flags, const char *const *includes,
                          unsigned include_count, const made_export_t *exports, unsigned export_count,
                          const made_import_t *imports, unsigned import_count) {
    memset(out, 0, 0x2000);
    uint64_t magic = Gsc_Magic;
    memcpy(out, &magic, 8);
    put32(out + Gsc_Name_Offset, Gsc_Header_Size);
    strcpy((char *)out + Gsc_Header_Size, name);
    put32(out + Gsc_Code_Offset, Code_At);
    put32(out + Gsc_Code_Size, Code_Size);
    out[Gsc_Flags] = flags;
    size_t at = Code_At + Code_Size;
    uint32_t texts[16];
    for (unsigned i = 0; i < include_count; i++) {
        texts[i] = (uint32_t)at;
        strcpy((char *)out + at, includes[i]);
        at += strlen(includes[i]) + 1;
    }
    put32(out + Gsc_Include_Offset, (uint32_t)at);
    out[Gsc_Include_Count] = (uint8_t)include_count;
    for (unsigned i = 0; i < include_count; i++, at += 4)
        put32(out + at, texts[i]);
    put32(out + Gsc_Exports_Offset, (uint32_t)at);
    put16(out + Gsc_Exports_Count, (uint16_t)export_count);
    for (unsigned i = 0; i < export_count; i++, at += Gsc_Export_Size) {
        put32(out + at + 4, exports[i].address);
        put32(out + at + 8, exports[i].name);
        put32(out + at + 12, exports[i].name_space);
        out[at + 16] = exports[i].params;
        out[at + 17] = exports[i].flags;
    }
    put32(out + Gsc_Imports_Offset, (uint32_t)at);
    put16(out + Gsc_Imports_Count, (uint16_t)import_count);
    for (unsigned i = 0; i < import_count; i++) {
        put32(out + at, imports[i].name);
        put32(out + at + 4, imports[i].name_space);
        put16(out + at + 8, (uint16_t)imports[i].call_count);
        out[at + 10] = imports[i].params;
        out[at + 11] = imports[i].flags;
        at += Gsc_Import_Size;
        for (unsigned c = 0; c < imports[i].call_count; c++, at += 4)
            put32(out + at, imports[i].calls[c]);
    }
    return at;
}

// what the game's linker does for an import that resolves to a script function: the address behind each call
static inline void link_import(uint8_t *obj, unsigned import_index, const uint8_t *function) {
    gsc_import_t import;
    unsigned index;
    size_t cursor = 0;
    while (gsc_obj_next_import(obj, &index, &cursor, &import)) {
        if (index - 1 != import_index)
            continue;
        for (unsigned c = 0; c < import.call_count; c++)
            memcpy(obj + gsc_call_slot(gsc_import_call(&import, c), import.flags), &function, 8);
    }
}

// a patch script asset: one section of detours, then the script; returns its size
static inline size_t make_gsic(uint8_t *out, const gsic_detour_t *detours, unsigned count, const uint8_t *body,
                        size_t body_size) {
    memset(out, 0, 16 + (size_t)count * Gsic_Entry_Size);
    put32(out, Gsic_Magic);
    put32(out + 4, 1);
    put32(out + 8, 0);
    put32(out + 12, count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = out + 16 + (size_t)i * Gsic_Entry_Size;
        put32(e, detours[i].replacement);
        put32(e + 4, detours[i].target_name_space);
        put32(e + 8, detours[i].target_name);
        put32(e + 12, detours[i].offset);
        put32(e + 16, detours[i].size);
        strcpy((char *)e + Gsic_Path_Offset, detours[i].target_script);
    }
    memcpy(out + 16 + (size_t)count * Gsic_Entry_Size, body, body_size);
    return 16 + (size_t)count * Gsic_Entry_Size + body_size;
}
