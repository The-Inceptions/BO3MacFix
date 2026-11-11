// Compiled scripts and All-around Enhancement 4's patch scripts, see gsc_obj.h.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "gsc_obj.h"

static uint16_t u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static uint32_t u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t gsc_hash(const char *name) {
    uint32_t hash = 0x4B9ACE2F;
    for (; *name != '\0'; name++) {
        int c = (signed char)*name; // the game sign-extends each character
        if (c >= 'A' && c <= 'Z')
            c += 0x20;
        hash = ((uint32_t)c ^ hash) * 0x1000193;
    }
    return hash * 0x1000193;
}

// whether a text starts at offset and ends inside the script
static bool text_inside(const uint8_t *obj, size_t size, uint32_t offset) {
    return offset < size && memchr(obj + offset, '\0', size - offset) != NULL;
}

bool gsc_obj_check(const uint8_t *obj, size_t size) {
    if (obj == NULL || size < Gsc_Header_Size)
        return false;
    uint64_t magic;
    memcpy(&magic, obj, sizeof(magic));
    if (magic != Gsc_Magic || !text_inside(obj, size, u32(obj + Gsc_Name_Offset)))
        return false;
    uint32_t code = u32(obj + Gsc_Code_Offset), code_size = u32(obj + Gsc_Code_Size);
    if (code > size || code_size > size - code)
        return false;

    uint32_t includes = u32(obj + Gsc_Include_Offset), include_count = obj[Gsc_Include_Count];
    if (includes > size || include_count * 4 > size - includes)
        return false;
    for (uint32_t i = 0; i < include_count; i++)
        if (!text_inside(obj, size, u32(obj + includes + i * 4)))
            return false;

    uint32_t exports = u32(obj + Gsc_Exports_Offset), export_count = u16(obj + Gsc_Exports_Count);
    if (exports > size || export_count * Gsc_Export_Size > size - exports)
        return false;
    for (uint32_t i = 0; i < export_count; i++) {
        uint32_t address = u32(obj + exports + i * Gsc_Export_Size + 4);
        if (address < code || address - code >= code_size)
            return false;
    }

    size_t at = u32(obj + Gsc_Imports_Offset);
    for (uint32_t i = u16(obj + Gsc_Imports_Count); i > 0; i--) {
        if (at > size || size - at < Gsc_Import_Size)
            return false;
        uint32_t calls = u16(obj + at + 8);
        uint8_t flags = obj[at + 11];
        at += Gsc_Import_Size;
        if ((size - at) / 4 < calls)
            return false;
        for (; calls > 0; calls--, at += 4) {
            // the opcode and the address behind it
            uint32_t call = u32(obj + at);
            if (call > size || size - call < 16 || gsc_call_slot(call, flags) > size - 8)
                return false;
        }
    }
    return true;
}

const char *gsc_obj_name(const uint8_t *obj) {
    return (const char *)obj + u32(obj + Gsc_Name_Offset);
}

unsigned gsc_obj_include_count(const uint8_t *obj) {
    return obj[Gsc_Include_Count];
}

const char *gsc_obj_include(const uint8_t *obj, unsigned index) {
    return (const char *)obj + u32(obj + u32(obj + Gsc_Include_Offset) + index * 4);
}

unsigned gsc_obj_export_count(const uint8_t *obj) {
    return u16(obj + Gsc_Exports_Count);
}

void gsc_obj_export(const uint8_t *obj, unsigned index, gsc_export_t *out) {
    const uint8_t *e = obj + u32(obj + Gsc_Exports_Offset) + (size_t)index * Gsc_Export_Size;
    out->address = u32(e + 4);
    out->name = u32(e + 8);
    out->name_space = u32(e + 12);
    out->params = e[16];
    out->flags = e[17];
}

bool gsc_obj_find_export(const uint8_t *obj, uint32_t name_space, uint32_t name, gsc_export_t *out) {
    for (unsigned i = 0, count = gsc_obj_export_count(obj); i < count; i++) {
        gsc_obj_export(obj, i, out);
        if (out->name == name && out->name_space == name_space)
            return true;
    }
    return false;
}

bool gsc_obj_next_import(const uint8_t *obj, unsigned *index, size_t *cursor, gsc_import_t *out) {
    if (*cursor == 0) {
        *cursor = u32(obj + Gsc_Imports_Offset);
        *index = 0;
    }
    if (*index >= u16(obj + Gsc_Imports_Count))
        return false;
    const uint8_t *import = obj + *cursor;
    out->name = u32(import);
    out->name_space = u32(import + 4);
    out->call_count = u16(import + 8);
    out->params = import[10];
    out->flags = import[11];
    out->calls = import + Gsc_Import_Size;
    *cursor += Gsc_Import_Size + (size_t)out->call_count * 4;
    ++*index;
    return true;
}

uint32_t gsc_import_call(const gsc_import_t *import, unsigned index) {
    return u32(import->calls + (size_t)index * 4);
}

#pragma mark - includes

bool gsc_include_is_directive(const char *text) {
    return strcasecmp(text, "#using") == 0 || strcasecmp(text, "#include") == 0 ||
           strcasecmp(text, "#using_animtree") == 0;
}

bool gsc_includes_strip(uint8_t *obj, gsc_include_backup_t *backup) {
    uint8_t *table = obj + u32(obj + Gsc_Include_Offset);
    backup->count = obj[Gsc_Include_Count];
    memcpy(backup->entries, table, (size_t)backup->count * 4);
    unsigned kept = 0;
    for (unsigned i = 0; i < backup->count; i++) {
        if (gsc_include_is_directive((const char *)obj + backup->entries[i]))
            continue;
        memcpy(table + kept * 4, &backup->entries[i], 4);
        kept++;
    }
    obj[Gsc_Include_Count] = (uint8_t)kept;
    backup->changed = kept != backup->count;
    return backup->changed;
}

void gsc_includes_restore(uint8_t *obj, const gsc_include_backup_t *backup) {
    if (!backup->changed)
        return;
    memcpy(obj + u32(obj + Gsc_Include_Offset), backup->entries, (size_t)backup->count * 4);
    obj[Gsc_Include_Count] = backup->count;
}

#pragma mark - whether the game would link it

// the linker's match of an import with an export: the names, and no more parameters passed than the function has
// unless it takes any number (&f passes none)
static bool export_takes(const gsc_export_t *e, const gsc_import_t *import) {
    if (e->name != import->name || e->name_space != import->name_space)
        return false;
    bool address_of = (import->flags & Gsc_Import_Type) == Gsc_Call_Address;
    return address_of || e->params >= import->params || (e->flags & Gsc_Export_Varargs) != 0;
}

// a function of the script itself, else a public one of an include
static bool script_function(const uint8_t *obj, const gsc_import_t *import, const gsc_link_env_t *env) {
    gsc_export_t e;
    for (unsigned i = 0, count = gsc_obj_export_count(obj); i < count; i++) {
        gsc_obj_export(obj, i, &e);
        if (export_takes(&e, import))
            return true;
    }
    for (unsigned i = 0, includes = gsc_obj_include_count(obj); i < includes; i++) {
        const uint8_t *included = env->script(env->context, gsc_obj_include(obj, i));
        if (included == NULL)
            continue;
        for (unsigned j = 0, count = gsc_obj_export_count(included); j < count; j++) {
            gsc_obj_export(included, j, &e);
            if (export_takes(&e, import) && (e.flags & Gsc_Export_Private) == 0)
                return true;
        }
    }
    return false;
}

bool gsc_obj_links(const uint8_t *obj, const gsc_link_env_t *env, char *why, size_t why_size) {
    for (unsigned i = 0, includes = gsc_obj_include_count(obj); i < includes; i++) {
        const char *include = gsc_obj_include(obj, i);
        if (env->script(env->context, include) == NULL) {
            snprintf(why, why_size, "the game has no script '%s'", include);
            return false;
        }
    }
    bool restricted = (obj[Gsc_Flags] & Gsc_Flag_Restricted) != 0;
    uint32_t no_namespace = gsc_hash(""), sys = gsc_hash("sys");
    gsc_import_t import;
    unsigned index;
    size_t cursor = 0;
    while (gsc_obj_next_import(obj, &index, &cursor, &import)) {
        unsigned type = import.flags & Gsc_Import_Type;
        bool dev = (import.flags & Gsc_Import_Dev) != 0;
        if (!restricted && env->skips_dev_calls && dev)
            continue;
        bool ok = type >= Gsc_Call_Address && type <= Gsc_Call_Method_Thread && script_function(obj, &import, env);
        // a builtin, for a call written without a namespace or with the builtins' own; a thread can't start on one
        bool builtin_allowed = import.name_space == no_namespace || import.name_space == sys ||
                               (import.flags & Gsc_Import_Any) != 0;
        if (!ok && builtin_allowed &&
            (type == Gsc_Call_Address || type == Gsc_Call_Function || type == Gsc_Call_Method)) {
            int kind = 0, min_params = 0, max_params = 0;
            bool found = false;
            if (type != Gsc_Call_Method)
                found = env->builtin(env->context, import.name, false, &kind, &min_params, &max_params);
            if (!found && type != Gsc_Call_Function)
                found = env->builtin(env->context, import.name, true, &kind, &min_params, &max_params);
            ok = found && !(kind == 2 && restricted) && !(kind == 1 && !dev) &&
                 (type == Gsc_Call_Address || (import.params >= min_params && import.params <= max_params));
        }
        if (!ok) {
            snprintf(why, why_size, "nothing has the function %08x::%08x it calls with %u parameters (import %u)",
                     import.name_space, import.name, import.params, index - 1);
            return false;
        }
    }
    return true;
}

#pragma mark - patch scripts

bool gsic_is(const uint8_t *asset, size_t size) {
    return asset != NULL && size >= 8 && u32(asset) == Gsic_Magic;
}

bool gsic_parse(const uint8_t *asset, size_t size, gsic_t *out) {
    memset(out, 0, sizeof(*out));
    if (!gsic_is(asset, size) || size > 0x4000000)
        return false;
    uint32_t sections = u32(asset + 4);
    if (sections > Gsic_Max_Sections)
        return false;
    size_t at = 8;
    for (uint32_t s = 0; s < sections; s++) {
        if (size - at < 8)
            return false;
        uint32_t instance = u32(asset + at), count = u32(asset + at + 4);
        at += 8;
        // only the server's scripts are patched
        if (instance != 0 || count > Gsic_Max_Detours - out->count || (size - at) / Gsic_Entry_Size < count)
            return false;
        out->entries[s] = asset + at;
        out->counts[s] = count;
        out->count += count;
        at += (size_t)count * Gsic_Entry_Size;
    }
    out->sections = sections;
    if (at >= size)
        return false;
    out->body = asset + at;
    out->body_size = size - at;
    if (!gsc_obj_check(out->body, out->body_size))
        return false;
    for (uint32_t i = 0; i < out->count; i++) {
        gsic_detour_t d;
        gsic_detour(out, i, &d);
        size_t path = strnlen(d.target_script, Gsic_Entry_Size - Gsic_Path_Offset);
        if (path == 0 || path > Gsic_Max_Path || d.target_name_space == 0 || d.target_name == 0 || d.size == 0 ||
            d.offset == 0 || d.offset > out->body_size || d.size > out->body_size - d.offset)
            return false;
    }
    return true;
}

void gsic_detour(const gsic_t *gsic, uint32_t index, gsic_detour_t *out) {
    uint32_t s = 0;
    while (index >= gsic->counts[s])
        index -= gsic->counts[s++];
    const uint8_t *e = gsic->entries[s] + (size_t)index * Gsic_Entry_Size;
    out->replacement = u32(e);
    out->target_name_space = u32(e + 4);
    out->target_name = u32(e + 8);
    out->offset = u32(e + 12);
    out->size = u32(e + 16);
    out->target_script = (const char *)e + Gsic_Path_Offset;
}

// a script name with '/' for '\' and in lower case, false when it doesn't fit
static bool normal_name(const char *name, char *out, size_t out_size) {
    size_t length = strlen(name);
    if (length >= out_size)
        return false;
    for (size_t i = 0; i <= length; i++) {
        char c = name[i];
        out[i] = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? (char)(c + 0x20) : c;
    }
    return true;
}

bool gsc_map_name_valid(const char *map) {
    size_t length = strlen(map);
    if (length < 3 || length > 63 || strncmp(map, "zm_", 3) != 0)
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = map[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_')
            return false;
    }
    return true;
}

bool gsc_mirror_name(const char *script, const char *map, char *out, size_t out_size) {
    char name[256];
    if (!normal_name(script, name, sizeof(name)))
        return false;
    size_t length = strlen(name);
    if (length <= 12 || strncmp(name, "scripts/", 8) != 0 || strcmp(name + length - 4, ".gsc") != 0 ||
        strncmp(name, "scripts/aae_common/", 19) == 0 || strncmp(name, "scripts/aae_level/", 18) == 0)
        return false;
    name[length - 4] = '\0';
    int written = map != NULL ? snprintf(out, out_size, "scripts/aae_level/%s/%s", map, name + 8)
                              : snprintf(out, out_size, "scripts/aae_common/%s", name + 8);
    return written > 0 && (size_t)written < out_size;
}

#pragma mark - detours

void gsc_rules_clear(gsc_rules_t *rules) {
    rules->count = 0;
    rules->resolved = 0;
}

bool gsc_rules_add(gsc_rules_t *rules, const gsic_t *gsic) {
    if (gsic->count > Gsic_Max_Detours - rules->count)
        return false;
    for (uint32_t i = 0; i < gsic->count; i++) {
        gsic_detour_t d;
        gsic_detour(gsic, i, &d);
        gsc_rule_t *rule = &rules->rules[rules->count];
        if (!normal_name(d.target_script, rule->target_script, sizeof(rule->target_script)))
            continue;
        rule->mirror = gsic->body;
        rule->offset = d.offset;
        rule->size = d.size;
        rule->target_name_space = d.target_name_space;
        rule->target_name = d.target_name;
        rule->target = NULL;
        rules->count++;
    }
    return true;
}

// the first of the resolved rules (in order) whose target isn't below the address
static uint32_t first_at(const gsc_rules_t *rules, const uint8_t *target) {
    uint32_t low = 0, high = rules->resolved;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (rules->rules[rules->order[middle]].target < target)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

uint32_t gsc_rules_resolve(gsc_rules_t *rules, const uint8_t *const *scripts, uint32_t script_count) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < rules->count; i++) {
        gsc_rule_t *rule = &rules->rules[i];
        if (rule->target != NULL)
            continue;
        for (uint32_t s = 0; s < script_count && rule->target == NULL; s++) {
            char name[sizeof(rule->target_script)];
            gsc_export_t e;
            if (!normal_name(gsc_obj_name(scripts[s]), name, sizeof(name)) || strcmp(name, rule->target_script) != 0)
                continue;
            // a replacement of itself would never end
            if (gsc_obj_find_export(scripts[s], rule->target_name_space, rule->target_name, &e) &&
                scripts[s] + e.address != rule->mirror + rule->offset)
                rule->target = scripts[s] + e.address;
            break;
        }
        if (rule->target == NULL)
            continue;
        found++;
        // into its place: after the rules with a lower target, and after the older ones with the same
        uint32_t at = first_at(rules, rule->target);
        while (at < rules->resolved && rules->rules[rules->order[at]].target == rule->target)
            at++;
        memmove(&rules->order[at + 1], &rules->order[at], (rules->resolved - at) * sizeof(rules->order[0]));
        rules->order[at] = (uint16_t)i;
        rules->resolved++;
    }
    return found;
}

uint32_t gsc_rules_patch(const gsc_rules_t *rules, uint8_t *obj) {
    if (rules->resolved == 0)
        return 0;
    uint32_t changed = 0;
    gsc_import_t import;
    unsigned index;
    size_t cursor = 0;
    while (gsc_obj_next_import(obj, &index, &cursor, &import)) {
        unsigned type = import.flags & Gsc_Import_Type;
        if (type < Gsc_Call_Address || type > Gsc_Call_Method_Thread)
            continue;
        for (unsigned c = 0; c < import.call_count; c++) {
            uint32_t call = gsc_import_call(&import, c);
            uint8_t *slot = obj + gsc_call_slot(call, import.flags);
            const uint8_t *target;
            memcpy(&target, slot, sizeof(target));
            uint32_t first = first_at(rules, target);
            if (first == rules->resolved || rules->rules[rules->order[first]].target != target)
                continue;
            bool inside = false;
            for (uint32_t at = first; at < rules->resolved && !inside; at++) {
                const gsc_rule_t *rule = &rules->rules[rules->order[at]];
                if (rule->target != target)
                    break;
                inside = obj == rule->mirror && call >= rule->offset && call - rule->offset < rule->size;
            }
            if (inside)
                continue;
            const gsc_rule_t *rule = &rules->rules[rules->order[first]];
            const uint8_t *replacement = rule->mirror + rule->offset;
            memcpy(slot, &replacement, sizeof(replacement));
            changed++;
        }
    }
    return changed;
}
