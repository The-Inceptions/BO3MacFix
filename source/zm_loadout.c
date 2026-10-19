// See zm_loadout.h.
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zm_loadout.h"

void loadout_clear(loadout_t *loadout) {
    memset(loadout, 0, sizeof(*loadout));
    loadout->held = -1;
}

// ---- names

uint32_t loadout_script_hash(const char *name) {
    uint32_t hash = 0x4B9ACE2F;
    for (; *name != '\0'; name++)
        hash = ((uint32_t)tolower((unsigned char)*name) ^ hash) * 0x1000193;
    return hash * 0x1000193;
}

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// a name of len characters, with '+' between parts when plus is true
static bool valid_name_len(const char *name, size_t len, bool plus) {
    if (len == 0 || len >= Loadout_Name_Size || name[0] == '+' || name[len - 1] == '+')
        return false;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '+' && plus && name[i + 1] != '+')
            continue;
        if (!name_char(name[i]))
            return false;
    }
    return true;
}

bool loadout_valid_name(const char *name) {
    return name != NULL && valid_name_len(name, strlen(name), true);
}

#define Weapon_Index_Bits 9
#define Weapon_Attachment_Bits 6
#define Weapon_Attachments 8

bool loadout_weapon_name(const char *variant, uint64_t weapon, const char *const *attachment_names, int attachment_count,
                         char *out, size_t out_size) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    if (variant == NULL || !valid_name_len(variant, strlen(variant), false))
        return false;
    size_t used = (size_t)snprintf(out, out_size, "%s", variant);
    for (int i = 0; i < Weapon_Attachments && used < out_size; i++) {
        int id = (int)((weapon >> (Weapon_Index_Bits + Weapon_Attachment_Bits * i)) & 0x3f);
        if (id == 0)
            continue;
        const char *name = id < attachment_count ? attachment_names[id] : NULL;
        if (name == NULL || !valid_name_len(name, strlen(name), false)) {
            out[0] = '\0';
            return false;
        }
        used += (size_t)snprintf(out + used, out_size - used, "+%s", name);
    }
    bool ok = used < out_size && used < Loadout_Name_Size && loadout_valid_name(out);
    if (!ok)
        out[0] = '\0';
    return ok;
}

bool loadout_other_name(const char *name, char *out, size_t out_size) {
    if (out_size == 0 || !loadout_valid_name(name))
        return false;
    const char *plus = strchr(name, '+');
    size_t root = plus != NULL ? (size_t)(plus - name) : strlen(name);
    const char *rest = name + root;
    bool zm = root > 3 && strncmp(name + root - 3, "_zm", 3) == 0;
    int len = zm ? snprintf(out, out_size, "%.*s%s", (int)(root - 3), name, rest)
                 : snprintf(out, out_size, "%.*s_zm%s", (int)root, name, rest);
    if (len < 0 || (size_t)len >= out_size || !loadout_valid_name(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

// ---- checkpoint.ini

static bool append(char *out, size_t out_size, size_t *used, const char *format, ...) __attribute__((format(printf, 4, 5)));
static bool append(char *out, size_t out_size, size_t *used, const char *format, ...) {
    if (*used >= out_size)
        return false;
    va_list args;
    va_start(args, format);
    int len = vsnprintf(out + *used, out_size - *used, format, args);
    va_end(args);
    if (len < 0 || (size_t)len >= out_size - *used) {
        out[*used] = '\0';
        return false;
    }
    *used += (size_t)len;
    return true;
}

bool loadout_format_guns(const loadout_t *loadout, char *out, size_t out_size) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < loadout->gun_count; i++) {
        const loadout_gun_t *gun = &loadout->guns[i];
        if (!loadout_valid_name(gun->name) || (gun->aat[0] != '\0' && !loadout_valid_name(gun->aat)) ||
            !append(out, out_size, &used, "%s%s:%" PRIx64 ":%u:%s", i > 0 ? "," : "", gun->name, gun->options,
                    gun->acvi, gun->aat)) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

bool loadout_format_perks(const loadout_t *loadout, char *out, size_t out_size) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < loadout->perk_count; i++) {
        if (!loadout_valid_name(loadout->perks[i]) ||
            !append(out, out_size, &used, "%s%s", i > 0 ? "," : "", loadout->perks[i])) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

// the text from start to end into out, if it fits
static bool copy_field(const char *start, const char *end, char *out, size_t out_size) {
    size_t len = (size_t)(end - start);
    if (len >= out_size)
        return false;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

// "name:options:acvi:aat" from start to end
static bool parse_gun(const char *start, const char *end, loadout_gun_t *gun) {
    const char *fields[4], *field_ends[4];
    const char *at = start;
    for (int i = 0; i < 4; i++) {
        const char *colon = memchr(at, ':', (size_t)(end - at));
        if ((colon == NULL) != (i == 3))
            return false;
        fields[i] = at;
        field_ends[i] = colon != NULL ? colon : end;
        at = colon != NULL ? colon + 1 : end;
    }
    char options[24], acvi[16];
    if (!copy_field(fields[0], field_ends[0], gun->name, sizeof(gun->name)) || !loadout_valid_name(gun->name) ||
        !copy_field(fields[1], field_ends[1], options, sizeof(options)) || options[0] == '\0' ||
        strlen(options) > 16 || !copy_field(fields[2], field_ends[2], acvi, sizeof(acvi)) || acvi[0] == '\0' ||
        strlen(acvi) > 10 || !copy_field(fields[3], field_ends[3], gun->aat, sizeof(gun->aat)) ||
        (gun->aat[0] != '\0' && !loadout_valid_name(gun->aat)))
        return false;
    for (const char *c = options; *c != '\0'; c++)
        if (!isxdigit((unsigned char)*c))
            return false;
    for (const char *c = acvi; *c != '\0'; c++)
        if (!isdigit((unsigned char)*c))
            return false;
    gun->options = strtoull(options, NULL, 16);
    unsigned long long value = strtoull(acvi, NULL, 10);
    if (value > UINT32_MAX)
        return false;
    gun->acvi = (uint32_t)value;
    return true;
}

bool loadout_parse(const char *guns, const char *in_hand, const char *perks, loadout_t *loadout) {
    loadout_clear(loadout);
    for (const char *item = guns; item != NULL && *item != '\0';) {
        const char *comma = strchr(item, ',');
        const char *end = comma != NULL ? comma : item + strlen(item);
        if (loadout->gun_count == Loadout_Max_Guns || !parse_gun(item, end, &loadout->guns[loadout->gun_count]))
            goto bad;
        loadout->gun_count++;
        item = comma != NULL ? comma + 1 : NULL;
        if (item != NULL && *item == '\0')
            goto bad; // a trailing comma
    }
    for (const char *item = perks; item != NULL && *item != '\0';) {
        const char *comma = strchr(item, ',');
        const char *end = comma != NULL ? comma : item + strlen(item);
        if (loadout->perk_count == Loadout_Max_Perks ||
            !copy_field(item, end, loadout->perks[loadout->perk_count], Loadout_Short_Size) ||
            !valid_name_len(loadout->perks[loadout->perk_count], strlen(loadout->perks[loadout->perk_count]), false))
            goto bad;
        loadout->perk_count++;
        item = comma != NULL ? comma + 1 : NULL;
        if (item != NULL && *item == '\0')
            goto bad;
    }
    if (in_hand != NULL && in_hand[0] != '\0') {
        if (!loadout_valid_name(in_hand))
            goto bad;
        for (int i = 0; i < loadout->gun_count && loadout->held < 0; i++)
            if (strcmp(loadout->guns[i].name, in_hand) == 0)
                loadout->held = i;
    }
    return true;
bad:
    loadout_clear(loadout);
    return false;
}

// ---- the script VM's variables

// a script variable, as the game lays them out (0x40 bytes each)
typedef struct {
    uint64_t value;     // a child's value; an object's first child (low half) and last child (high half)
    uint32_t type;      // Var_*
    uint32_t pad0;
    uint8_t name_type;  // a child's key type in the low 3 bits: 1 and 2 numbers, 3 strings, 5 weapon objects
    uint8_t pad1[7];
    uint64_t entref;    // an entity or weapon object's entity number or Weapon
    uint16_t classnum;  // which class that object is of: 0 entities, 5 weapons
    uint16_t client;
    uint32_t pad2;
    uint64_t key;       // a child's key (a field's hash, an array's index, string or weapon object)
    uint32_t next;      // the parent's next child
    uint32_t prev;
    uint32_t parent;
    uint32_t hash_next; // the next variable in its hash bucket
} vm_var_t;
_Static_assert(sizeof(vm_var_t) == 0x40, "a script variable is 0x40 bytes");

#define VarGlob_Buckets 0x0
#define VarGlob_Vars 0x80
#define Class_Size 0x20
#define Class_Object 0x4
#define Class_PerClient 0x18
#define String_Size 0x1c
#define String_Text 0x4
#define Var_Pointer 1
#define Var_String 2
#define Var_Hash 5
#define Var_Entity 0x18
#define Var_Array 0x19
#define Name_Weapon 5
#define Class_Entity 0
#define Class_Weapon 5
#define Max_Chain 4096    // variables walked in one hash bucket
#define Max_Children 1024 // an array's elements walked

static bool vm_read(const loadout_vm_t *vm, uint64_t address, void *out, size_t size) {
    return address != 0 && vm->read != NULL && vm->read(address, out, size);
}

static bool vm_var(const loadout_vm_t *vm, uint32_t id, vm_var_t *out) {
    uint64_t vars = 0;
    return id != 0 && vm_read(vm, vm->var_glob + VarGlob_Vars, &vars, sizeof(vars)) &&
           vm_read(vm, vars + (uint64_t)id * sizeof(vm_var_t), out, sizeof(*out));
}

uint32_t loadout_vm_find(const loadout_vm_t *vm, uint32_t object, uint64_t key) {
    uint64_t buckets = 0;
    if (object == 0 || !vm_read(vm, vm->var_glob + VarGlob_Buckets, &buckets, sizeof(buckets)))
        return 0;
    // the game's FindVariable: the bucket is the key plus 0x65 times the parent, in 16 bits
    uint16_t bucket = (uint16_t)((uint32_t)key + object * 0x65u);
    uint32_t id = 0;
    if (!vm_read(vm, buckets + (uint64_t)bucket * sizeof(uint32_t), &id, sizeof(id)))
        return 0;
    for (int steps = 0; id != 0 && steps < Max_Chain; steps++) {
        vm_var_t var;
        if (!vm_var(vm, id, &var))
            return 0;
        if (var.parent == object && var.key == key)
            return id;
        id = var.hash_next;
    }
    return 0;
}

uint32_t loadout_vm_entity(const loadout_vm_t *vm, int entnum) {
    uint64_t classes = 0;
    uint32_t class_object = 0;
    uint8_t per_client = 0;
    if (entnum < 0 || !vm_read(vm, vm->class_map, &classes, sizeof(classes)) ||
        !vm_read(vm, classes + Class_Entity * Class_Size + Class_Object, &class_object, sizeof(class_object)) ||
        !vm_read(vm, classes + Class_Entity * Class_Size + Class_PerClient, &per_client, sizeof(per_client)) ||
        per_client != 0)
        return 0;
    // the entity class's object has a child per entity number pointing at the entity's object
    vm_var_t child, object;
    uint32_t id = loadout_vm_find(vm, class_object, (uint64_t)entnum);
    if (id == 0 || !vm_var(vm, id, &child) || child.type != Var_Pointer)
        return 0;
    uint32_t entity = (uint32_t)child.value;
    if (!vm_var(vm, entity, &object) || object.type != Var_Entity || object.classnum != Class_Entity ||
        object.entref != (uint64_t)entnum)
        return 0;
    return entity;
}

uint32_t loadout_vm_array_field(const loadout_vm_t *vm, uint32_t entity, const char *field) {
    vm_var_t var, array;
    uint32_t id = loadout_vm_find(vm, entity, loadout_script_hash(field));
    if (id == 0 || !vm_var(vm, id, &var) || var.type != Var_Pointer)
        return 0;
    uint32_t object = (uint32_t)var.value;
    return vm_var(vm, object, &array) && array.type == Var_Array ? object : 0;
}

// the text of string id, if it's a name
static bool vm_string(const loadout_vm_t *vm, uint32_t id, char out[Loadout_Short_Size]) {
    uint64_t table = 0;
    if (id == 0 || !vm_read(vm, vm->strings, &table, sizeof(table)) || table == 0)
        return false;
    uint64_t text = table + (uint64_t)id * String_Size + String_Text;
    size_t len = 0;
    for (; len < Loadout_Short_Size; len++) {
        char c;
        if (!vm_read(vm, text + len, &c, 1))
            return false;
        out[len] = c;
        if (c == '\0')
            break;
    }
    return len > 0 && len < Loadout_Short_Size && valid_name_len(out, len, false);
}

// the alternate ammo types the zombies scripts register (aats/_zm_aat_*.gsc), for a value kept as a hash
static const char *const known_aats[] = { "zm_aat_blast_furnace", "zm_aat_dead_wire", "zm_aat_fire_works",
                                          "zm_aat_thunder_wall", "zm_aat_turned" };

// a child's value as a name: a string, or a hash of one of the known ammo types
static bool vm_value_name(const loadout_vm_t *vm, const vm_var_t *var, char out[Loadout_Short_Size]) {
    if (var->type == Var_String)
        return vm_string(vm, (uint32_t)var->value, out);
    if (var->type == Var_Hash) {
        for (size_t i = 0; i < sizeof(known_aats) / sizeof(known_aats[0]); i++) {
            if ((uint32_t)var->value == loadout_script_hash(known_aats[i])) {
                snprintf(out, Loadout_Short_Size, "%s", known_aats[i]);
                return true;
            }
        }
    }
    return false;
}

int loadout_vm_read_aats(const loadout_vm_t *vm, uint32_t entity, loadout_aat_t *out, int max) {
    uint32_t array = loadout_vm_array_field(vm, entity, "aat");
    vm_var_t object;
    if (array == 0 || !vm_var(vm, array, &object))
        return -1;
    int count = 0;
    uint32_t id = (uint32_t)object.value;
    for (int steps = 0; id != 0 && steps < Max_Children && count < max; steps++) {
        vm_var_t child, weapon;
        if (!vm_var(vm, id, &child) || child.parent != array)
            return count > 0 ? count : -1;
        // keyed by the weapon object (aat::acquire: self.aat[weapon] = name)
        if ((child.name_type & 7) == Name_Weapon && vm_var(vm, (uint32_t)child.key, &weapon) &&
            weapon.type == Var_Entity && weapon.classnum == Class_Weapon && (weapon.entref & 0x1ff) != 0 &&
            vm_value_name(vm, &child, out[count].aat) && strcmp(out[count].aat, "none") != 0) {
            out[count].weapon = weapon.entref;
            count++;
        }
        id = child.next;
    }
    return count;
}

int loadout_vm_read_strings(const loadout_vm_t *vm, uint32_t array, char (*out)[Loadout_Short_Size], int max) {
    vm_var_t object;
    if (array == 0 || !vm_var(vm, array, &object) || object.type != Var_Array)
        return -1;
    int count = 0;
    uint32_t id = (uint32_t)object.value;
    for (int steps = 0; id != 0 && steps < Max_Children && count < max; steps++) {
        vm_var_t child;
        if (!vm_var(vm, id, &child) || child.parent != array)
            return count > 0 ? count : -1;
        if (child.type == Var_String && vm_string(vm, (uint32_t)child.value, out[count])) {
            bool seen = false;
            for (int i = 0; i < count && !seen; i++)
                seen = strcmp(out[i], out[count]) == 0;
            if (!seen)
                count++;
        }
        id = child.next;
    }
    return count;
}

// ---- restoring

static void note_failure(loadout_result_t *result, const char *what, const char *name) {
    result->failed++;
    size_t used = strlen(result->failures);
    if (used + 1 < sizeof(result->failures))
        snprintf(result->failures + used, sizeof(result->failures) - used, "%s%s %s", used > 0 ? ", " : "", what,
                 name);
}

static bool contains(const uint64_t *weapons, int count, uint64_t weapon) {
    for (int i = 0; i < count; i++)
        if (weapons[i] == weapon)
            return true;
    return false;
}

void loadout_restore(const loadout_t *saved, const loadout_ops_t *ops, loadout_result_t *result) {
    memset(result, 0, sizeof(*result));
    void *context = ops->context;
    uint64_t wanted[Loadout_Max_Guns] = { 0 };
    for (int i = 0; i < saved->gun_count; i++) {
        wanted[i] = ops->weapon_for_name(context, saved->guns[i].name);
        if (wanted[i] == 0)
            note_failure(result, "no weapon", saved->guns[i].name);
    }
    // the perks first: with Mule Kick (specialty_additionalprimaryweapon) a third gun has room, and the game's weapon
    // limit would take one given before it
    for (int i = 0; i < saved->perk_count; i++) {
        if (ops->has_perk(context, saved->perks[i]))
            result->perks_had++;
        else if (ops->give_perk(context, saved->perks[i]))
            result->perks++;
        else
            note_failure(result, "no perk", saved->perks[i]);
    }
    // nothing to give (a save without guns, or none this game has): the guns the game started with stay
    int resolved = 0;
    for (int i = 0; i < saved->gun_count; i++)
        resolved += wanted[i] != 0;
    uint64_t held[16];
    int held_count = resolved > 0 ? ops->primaries(context, held, (int)(sizeof(held) / sizeof(held[0]))) : 0;
    if (held_count < 0)
        held_count = 0;
    // the starting guns that weren't saved go first, so the saved ones have room
    for (int i = 0; i < held_count; i++) {
        if (contains(wanted, saved->gun_count, held[i]))
            continue;
        if (ops->take(context, held[i]))
            result->taken++;
        else
            note_failure(result, "couldn't take", "a starting gun");
    }
    for (int i = 0; i < saved->gun_count; i++) {
        const loadout_gun_t *gun = &saved->guns[i];
        if (wanted[i] == 0 || contains(wanted, i, wanted[i]))
            continue;
        if (contains(held, held_count, wanted[i]))
            result->kept++;
        else if (ops->give(context, wanted[i], gun->options, gun->acvi))
            result->given++;
        else
            note_failure(result, "couldn't give", gun->name);
    }
    for (int i = 0; i < saved->gun_count; i++) {
        const loadout_gun_t *gun = &saved->guns[i];
        if (wanted[i] == 0 || gun->aat[0] == '\0' || contains(wanted, i, wanted[i]))
            continue;
        if (ops->acquire_aat(context, wanted[i], gun->aat))
            result->aats++;
        else
            note_failure(result, "no ammo type", gun->aat);
    }
    // the gun that was in hand, or the first one if none was (holding a grenade, say), so the hands aren't left empty
    // after the starting pistol went
    int in_hand = saved->held >= 0 && saved->held < saved->gun_count && wanted[saved->held] != 0 ? saved->held : -1;
    for (int i = 0; in_hand < 0 && i < saved->gun_count; i++)
        if (wanted[i] != 0)
            in_hand = i;
    if (in_hand >= 0)
        ops->switch_to(context, wanted[in_hand]);
}

void loadout_result_text(const loadout_result_t *result, char *out, size_t out_size) {
    snprintf(out, out_size, "took %d, gave %d guns (%d held already), %d ammo types, %d perks (%d had already), %d "
             "failed%s%s", result->taken, result->given, result->kept, result->aats, result->perks, result->perks_had,
             result->failed, result->failed > 0 ? ": " : "", result->failures);
}

// ---- AAE's Repack-a-Punch levels

#define Var_Int 7
#define Name_String 3
#define Pap_Key_Size (Loadout_Name_Size + 32) // a guid and a root weapon name

// the length of a gun name's root weapon name (before its attachments)
static size_t root_len(const char *name) {
    const char *plus = strchr(name, '+');
    return plus != NULL ? (size_t)(plus - name) : strlen(name);
}

bool loadout_format_pap_levels(const loadout_t *loadout, char *out, size_t out_size) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < loadout->gun_count; i++) {
        const loadout_gun_t *gun = &loadout->guns[i];
        if (gun->pap_level <= 0)
            continue;
        if (gun->pap_level > Loadout_Max_Pap_Level || !loadout_valid_name(gun->name) ||
            !append(out, out_size, &used, "%s%s:%d", used > 0 ? "," : "", gun->name, gun->pap_level)) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

bool loadout_parse_pap_levels(const char *value, loadout_t *loadout) {
    int levels[Loadout_Max_Guns];
    for (int i = 0; i < loadout->gun_count; i++)
        levels[i] = loadout->guns[i].pap_level;
    for (const char *start = value; start != NULL && *start != '\0';) {
        const char *end = strchr(start, ',');
        if (end == NULL)
            end = start + strlen(start);
        const char *colon = memchr(start, ':', (size_t)(end - start));
        // name:level, the level one digit
        if (colon == NULL || colon == start || end - colon != 2 || colon[1] < '1' ||
            colon[1] > '0' + Loadout_Max_Pap_Level || !valid_name_len(start, (size_t)(colon - start), true))
            return false;
        for (int i = 0; i < loadout->gun_count; i++)
            if (strlen(loadout->guns[i].name) == (size_t)(colon - start) &&
                memcmp(loadout->guns[i].name, start, (size_t)(colon - start)) == 0)
                levels[i] = colon[1] - '0';
        start = *end == ',' ? end + 1 : end;
        if (*end == ',' && *start == '\0')
            return false;
    }
    for (int i = 0; i < loadout->gun_count; i++)
        loadout->guns[i].pap_level = levels[i];
    return true;
}

uint32_t loadout_vm_level(const loadout_vm_t *vm) {
    uint32_t id = 0;
    vm_var_t object;
    if (vm->level_id == 0 || !vm_read(vm, vm->level_id, &id, sizeof(id)) || !vm_var(vm, id, &object))
        return 0;
    return id;
}

// the text of string id: printable characters, at least one, shorter than out_size
static bool vm_text(const loadout_vm_t *vm, uint32_t id, char *out, size_t out_size) {
    uint64_t table = 0;
    if (id == 0 || out_size == 0 || !vm_read(vm, vm->strings, &table, sizeof(table)) || table == 0)
        return false;
    uint64_t text = table + (uint64_t)id * String_Size + String_Text;
    for (size_t len = 0; len < out_size; len++) {
        char c;
        if (!vm_read(vm, text + len, &c, 1))
            return false;
        out[len] = c;
        if (c == '\0')
            return len > 0;
        if (c <= ' ' || c > '~')
            return false;
    }
    return false;
}

bool loadout_vm_guid(const loadout_vm_t *vm, uint32_t entity, char *out, size_t out_size) {
    vm_var_t var;
    uint32_t id = loadout_vm_find(vm, entity, loadout_script_hash("guid"));
    if (out_size == 0 || id == 0 || !vm_var(vm, id, &var))
        return false;
    if (var.type == Var_Int) {
        int len = snprintf(out, out_size, "%d", (int32_t)(uint32_t)var.value);
        return len > 0 && (size_t)len < out_size;
    }
    return var.type == Var_String && vm_text(vm, (uint32_t)var.value, out, out_size);
}

// whether text is a GSC int's decimal
static bool decimal(const char *text, size_t len) {
    size_t i = len > 0 && text[0] == '-' ? 1 : 0;
    if (i == len)
        return false;
    for (; i < len; i++)
        if (!isdigit((unsigned char)text[i]))
            return false;
    return true;
}

int loadout_vm_read_pap_levels(const loadout_vm_t *vm, const char *guid, loadout_t *loadout) {
    uint32_t array = loadout_vm_array_field(vm, loadout_vm_level(vm), "pap_lvup");
    vm_var_t object;
    if (array == 0 || !vm_var(vm, array, &object))
        return -1;
    int levels[Loadout_Max_Guns] = { 0 };
    char prefix[Pap_Key_Size] = "";
    bool mixed = false;
    uint32_t id = (uint32_t)object.value;
    for (int steps = 0; id != 0 && steps < Max_Children; steps++) {
        vm_var_t child;
        char key[Pap_Key_Size];
        if (!vm_var(vm, id, &child) || child.parent != array)
            break;
        id = child.next;
        int level = (int32_t)(uint32_t)child.value;
        if ((child.name_type & 7) != Name_String || child.type != Var_Int || level < 1 ||
            level > Loadout_Max_Pap_Level || !vm_text(vm, (uint32_t)child.key, key, sizeof(key)))
            continue;
        size_t key_len = strlen(key);
        for (int i = 0; i < loadout->gun_count; i++) {
            const char *name = loadout->guns[i].name;
            size_t len = root_len(name);
            if (key_len <= len || memcmp(key + key_len - len, name, len) != 0)
                continue;
            size_t prefix_len = key_len - len;
            if (guid != NULL) {
                if (strlen(guid) == prefix_len && memcmp(key, guid, prefix_len) == 0)
                    levels[i] = level;
            } else if (decimal(key, prefix_len)) {
                // without the guid, every gun's key has to have the same one (another player's would differ)
                if (prefix[0] == '\0')
                    snprintf(prefix, sizeof(prefix), "%.*s", (int)prefix_len, key);
                else if (strlen(prefix) != prefix_len || memcmp(prefix, key, prefix_len) != 0)
                    mixed = true;
                levels[i] = level;
            }
        }
    }
    int count = 0;
    for (int i = 0; i < loadout->gun_count; i++) {
        loadout->guns[i].pap_level = mixed ? 0 : levels[i];
        count += loadout->guns[i].pap_level > 0;
    }
    return count;
}
