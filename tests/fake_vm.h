// A made-up server script VM for the tests, laid out as the game's (see zm_loadout.c): the variable globals, a class
// table, the hash buckets, the variables (0x40 bytes each) and the string table, in one block. The code under test sees
// the block at `address`: the block itself, or a made-up address its own read function maps back.
#pragma once
#include <stdint.h>
#include <string.h>
#include "../source/zm_loadout.h"

#define FakeVm_Glob 0x0
#define FakeVm_ClassMap 0x100 // what the class table pointer is
#define FakeVm_Strings 0x108  // what the string table pointer is
#define FakeVm_Classes 0x200
#define FakeVm_Buckets 0x1000
#define FakeVm_Vars (FakeVm_Buckets + 65536 * 4)
#define FakeVm_Var_Count 512
#define FakeVm_String_Table (FakeVm_Vars + FakeVm_Var_Count * 0x40)
#define FakeVm_Size (FakeVm_String_Table + 0x4000)

typedef struct {
    uint8_t *memory; // FakeVm_Size bytes
    uint64_t address;
    uint32_t next_var, next_string;
} fake_vm_t;

static inline void fake_vm_put(fake_vm_t *vm, uint64_t offset, const void *value, size_t size) {
    memcpy(vm->memory + offset, value, size);
}
static inline void fake_vm_put64(fake_vm_t *vm, uint64_t offset, uint64_t value) {
    fake_vm_put(vm, offset, &value, 8);
}
static inline void fake_vm_put32(fake_vm_t *vm, uint64_t offset, uint32_t value) {
    fake_vm_put(vm, offset, &value, 4);
}
static inline uint32_t fake_vm_get32(fake_vm_t *vm, uint64_t offset) {
    uint32_t value;
    memcpy(&value, vm->memory + offset, 4);
    return value;
}
static inline uint64_t fake_vm_var(uint32_t id) {
    return FakeVm_Vars + (uint64_t)id * 0x40;
}

// an empty VM: its globals, class table and string table pointers set up for address
static inline void fake_vm_init(fake_vm_t *vm, uint8_t *memory, uint64_t address) {
    memset(memory, 0, FakeVm_Size);
    vm->memory = memory;
    vm->address = address;
    vm->next_var = 1;
    vm->next_string = 1;
    fake_vm_put64(vm, FakeVm_Glob + 0x0, address + FakeVm_Buckets);
    fake_vm_put64(vm, FakeVm_Glob + 0x80, address + FakeVm_Vars);
    fake_vm_put64(vm, FakeVm_ClassMap, address + FakeVm_Classes);
    fake_vm_put64(vm, FakeVm_Strings, address + FakeVm_String_Table);
}

static inline uint32_t fake_vm_object(fake_vm_t *vm, uint32_t type) {
    uint32_t id = vm->next_var++;
    fake_vm_put32(vm, fake_vm_var(id) + 0x8, type);
    return id;
}

// an entity (class 0) or weapon (class 5) object
static inline uint32_t fake_vm_entity(fake_vm_t *vm, uint16_t classnum, uint64_t entref) {
    uint32_t id = fake_vm_object(vm, 0x18);
    fake_vm_put64(vm, fake_vm_var(id) + 0x18, entref);
    fake_vm_put(vm, fake_vm_var(id) + 0x20, &classnum, 2);
    return id;
}

// a child of parent with key, after its other children and first in its hash bucket
static inline uint32_t fake_vm_child(fake_vm_t *vm, uint32_t parent, uint64_t key, uint8_t name_type, uint64_t value,
                                     uint32_t type) {
    uint32_t id = vm->next_var++;
    uint64_t at = fake_vm_var(id);
    fake_vm_put64(vm, at, value);
    fake_vm_put32(vm, at + 0x8, type);
    vm->memory[at + 0x10] = name_type;
    fake_vm_put64(vm, at + 0x28, key);
    fake_vm_put32(vm, at + 0x38, parent);
    uint32_t last = fake_vm_get32(vm, fake_vm_var(parent) + 0x4);
    if (last == 0)
        fake_vm_put32(vm, fake_vm_var(parent), id);
    else
        fake_vm_put32(vm, fake_vm_var(last) + 0x30, id);
    fake_vm_put32(vm, at + 0x34, last);
    fake_vm_put32(vm, fake_vm_var(parent) + 0x4, id);
    uint16_t bucket = (uint16_t)((uint32_t)key + parent * 0x65u);
    fake_vm_put32(vm, at + 0x3c, fake_vm_get32(vm, FakeVm_Buckets + bucket * 4u));
    fake_vm_put32(vm, FakeVm_Buckets + bucket * 4u, id);
    return id;
}

static inline uint32_t fake_vm_string(fake_vm_t *vm, const char *text) {
    uint32_t id = vm->next_string;
    size_t len = strlen(text);
    memcpy(vm->memory + FakeVm_String_Table + id * 0x1c + 4, text, len + 1);
    vm->next_string += (uint32_t)((len + 4 + 0x1c) / 0x1c); // long strings take more than one slot, as in the game
    return id;
}

// the entity class's object, with a child for entity entnum pointing at a new entity object, which it returns
static inline uint32_t fake_vm_player(fake_vm_t *vm, int entnum) {
    uint32_t entity_class = fake_vm_get32(vm, FakeVm_Classes + 4);
    if (entity_class == 0) {
        entity_class = fake_vm_object(vm, 0x14);
        fake_vm_put32(vm, FakeVm_Classes + 4, entity_class);
    }
    uint32_t player = fake_vm_entity(vm, 0, (uint64_t)entnum);
    fake_vm_child(vm, entity_class, (uint64_t)entnum, 1, player, 1);
    return player;
}

// a new array as entity's field
static inline uint32_t fake_vm_array_field(fake_vm_t *vm, uint32_t entity, const char *field) {
    uint32_t array = fake_vm_object(vm, 0x19);
    fake_vm_child(vm, entity, loadout_script_hash(field), 4, array, 1);
    return array;
}

// self.aat[weapon] = aat, as aat::acquire does
static inline void fake_vm_add_aat(fake_vm_t *vm, uint32_t aat_array, uint64_t weapon, const char *aat) {
    uint32_t object = fake_vm_entity(vm, 5, weapon);
    fake_vm_child(vm, aat_array, object, 5, fake_vm_string(vm, aat), 2);
}

// array[index] = text
static inline void fake_vm_add_string(fake_vm_t *vm, uint32_t array, uint64_t index, const char *text) {
    fake_vm_child(vm, array, index, 1, fake_vm_string(vm, text), 2);
}
