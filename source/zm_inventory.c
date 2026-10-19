// See zm_inventory.h.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dobby.h"
#include "offsets.h"
#include "zm_inventory.h"

#define Weapon_Index_Mask 0x1ff // a Weapon's low 9 bits are its index in the weapon tables, 0 for none
#define Allow_Ms 10000

static uint64_t base;
static atomic_bool hooked;
static atomic_int allowed;
static atomic_llong allowed_until;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// copies size bytes at address into out; false, without a crash, where nothing is mapped
static bool safe_read(uint64_t address, void *out, size_t size) {
    mach_vm_size_t got = 0;
    return address != 0 &&
           mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)out, &got) == KERN_SUCCESS &&
           got == size;
}

// g_entities, whose first entity is the local player's in a game this Mac hosts
static uint64_t entities(void) {
    uint64_t address = 0;
    return base != 0 && safe_read(base + ADDR_g_entities_Ptr, &address, sizeof(address)) ? address : 0;
}

// the weapon's name (like ray_gun_upgraded) and inventory type, from its WeaponVariantDef
static bool weapon_info(uint64_t weapon, char *name, size_t name_size, int *type) {
    uint32_t index = (uint32_t)(weapon & Weapon_Index_Mask);
    int32_t last = 0;
    uint64_t variant = 0, text = 0, def = 0;
    if (index == 0 || !safe_read(base + ADDR_bg_lastWeaponIndex, &last, sizeof(last)) || index > (uint32_t)last ||
        !safe_read(base + ADDR_bg_weaponVariantDefs + index * sizeof(uint64_t), &variant, sizeof(variant)) ||
        !safe_read(variant, &text, sizeof(text)) || !safe_read(variant + WeaponVariantDef_WeaponDef, &def, sizeof(def)) ||
        !safe_read(def + WeaponDef_InventoryType, type, sizeof(*type)))
        return false;
    // a byte at a time, so the end of the name's page doesn't matter
    size_t len = 0;
    for (; len + 1 < name_size; len++) {
        char c;
        if (!safe_read(text + len, &c, 1))
            return false;
        if (c == '\0')
            break;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
        name[len] = c;
    }
    name[len] = '\0';
    return len > 0 && len + 1 < name_size;
}

bool zm_inventory_read(char *out, size_t out_size) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    uint64_t first = entities(), client = 0, current = 0;
    if (first == 0 || !safe_read(first + gentity_Client, &client, sizeof(client)) || client == 0 ||
        !safe_read(client + gclient_Weapon, &current, sizeof(current)))
        return false;
    uint64_t held[gclient_HeldWeapon_Count];
    for (int i = 0; i < gclient_HeldWeapon_Count; i++)
        if (!safe_read(client + gclient_HeldWeapons + (uint64_t)i * gclient_HeldWeapon_Size, &held[i], sizeof(held[i])))
            return false;
    size_t used = 0;
    // the one in hand, then the rest
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < gclient_HeldWeapon_Count; i++) {
            if ((held[i] & Weapon_Index_Mask) == 0 || (pass == 0) != (held[i] == current))
                continue;
            char name[72];
            int type = 0;
            if (!weapon_info(held[i], name, sizeof(name), &type))
                continue;
            int len = snprintf(out + used, out_size - used, "%s%s:%d", used > 0 ? "," : "", name, type);
            if (len < 0 || (size_t)len >= out_size - used) {
                out[used] = '\0';
                return used > 0;
            }
            used += (size_t)len;
        }
    }
    return true;
}

// CheatsOk is hooked only when a resumed game first needs it: a pre-patched copy without a slot for it (one made by an
// older BO3MacFix) gets the hook written into the game's code, which brings back Rosetta's JIT, so not every game
// should pay for it
static void hook_cheats_ok(void);

void zm_inventory_allow_cheats(int count) {
    if (!atomic_exchange(&hooked, true))
        hook_cheats_ok();
    atomic_store(&allowed_until, now_ms() + Allow_Ms);
    atomic_store(&allowed, count);
}

// CheatsOk(ent): whether ent may use cheat commands (sv_cheats is on and it's alive). BO3MacFix's own give and take
// commands for the local player get through while zm_inventory_allow_cheats says so; everything else is the game's.
install_hook_name(CheatsOk, int, void *ent) {
    if (atomic_load(&allowed) > 0 && now_ms() < atomic_load(&allowed_until) && ent != NULL &&
        (uint64_t)ent == entities()) {
        int health = 0;
        if (safe_read((uint64_t)ent + gentity_Health, &health, sizeof(health)) && health > 0) {
            atomic_fetch_sub(&allowed, 1);
            printf("BO3MacFix: zm inventory: let BO3MacFix's command past the cheat check\n");
            return 1;
        }
    }
    return orig_CheatsOk(ent);
}

void zm_inventory_init(uint64_t game_base) {
    base = game_base;
}

static void hook_cheats_ok(void) {
    if (base != 0)
        install_hook_CheatsOk((void *)(base + ADDR_CheatsOk));
}
