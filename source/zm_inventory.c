// See zm_inventory.h.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dobby.h"
#include "offsets.h"
#include "zm_inventory.h"
#include "zm_loadout.h"

#define Weapon_Index_Mask 0x1ff // a Weapon's low 9 bits are its index in the weapon tables, 0 for none
#define Allowed_Max 8            // give and take commands let through at once (a resume sends two or three)

static uint64_t base;
static void (*SV_Cmd_ArgvBuffer)(int arg, char *buffer, int size); // the client command's arguments
// BO3MacFix's own give and take commands, each let past the cheat check once, whenever the game runs it: it holds
// them while it's paused (the pause menu open as the resume sends them), and they ran too late for a 10 s allowance
typedef struct {
    bool give;
    char name[64];
} allowed_command_t;
static allowed_command_t allowed[Allowed_Max];
static int allowed_count;      // how many of them the game hasn't run yet
static long long allowed_at;   // when they were allowed, for the log
static os_unfair_lock allowed_lock = OS_UNFAIR_LOCK_INIT;

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

// whether this Mac runs the game's server (a solo game, or one it hosts): the sv_running dvar, read through safe_read.
// Without it g_entities and the server's script VM are what the last game here left (a match hosted elsewhere), so
// nothing is read from them, and the game's own functions aren't called on them.
static bool server_running(void) {
    uint64_t dvar = 0;
    uint32_t type = 0;
    uint8_t value = 0;
    return base != 0 && safe_read(base + ADDR_sv_running, &dvar, sizeof(dvar)) &&
           safe_read(dvar + dvar_Type, &type, sizeof(type)) && type == 1 &&
           safe_read(dvar + dvar_Value, &value, sizeof(value)) && value != 0;
}

// a name at text, if it's a weapon name (letters, digits and '_')
static bool read_name(uint64_t text, char *name, size_t name_size) {
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

// the name the server looks the weapon up by (G_GetWeaponForName, the give command, GetWeapon): its WeaponVariantDef's
// second name, which zombies' weapons have without "_zm" (ray_gun_upgraded for ray_gun_upgraded_zm)
static bool weapon_script_name(uint64_t weapon, char *name, size_t name_size) {
    uint32_t index = (uint32_t)(weapon & Weapon_Index_Mask);
    int32_t last = 0;
    uint64_t variant = 0, text = 0;
    return index != 0 && safe_read(base + ADDR_bg_lastWeaponIndex, &last, sizeof(last)) && index <= (uint32_t)last &&
           safe_read(base + ADDR_bg_weaponVariantDefs + index * sizeof(uint64_t), &variant, sizeof(variant)) &&
           safe_read(variant + WeaponVariantDef_ScriptName, &text, sizeof(text)) && read_name(text, name, name_size);
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
    return read_name(text, name, name_size);
}

bool zm_inventory_read(char *out, size_t out_size) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    if (!server_running())
        return false;
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

static void drop_shield_health(const char *why);

int zm_inventory_allow_commands(const char *commands) {
    // none: a new match (checkpoint.lua), where a shield's health still waiting is the last game's
    if (commands == NULL || commands[0] == '\0')
        drop_shield_health("a new match");
    os_unfair_lock_lock(&allowed_lock);
    allowed_count = 0;
    allowed_at = now_ms();
    for (const char *line = commands; line != NULL && *line != '\0' && allowed_count < Allowed_Max;) {
        size_t len = strcspn(line, "\n");
        bool give = len > 5 && strncmp(line, "give ", 5) == 0, take = len > 5 && strncmp(line, "take ", 5) == 0;
        size_t name_len = len - 5;
        // "give name" or "take name", nothing after the name
        if ((give || take) && name_len < sizeof(allowed[0].name) && memchr(line + 5, ' ', name_len) == NULL) {
            allowed[allowed_count].give = give;
            memcpy(allowed[allowed_count].name, line + 5, name_len);
            allowed[allowed_count].name[name_len] = '\0';
            allowed_count++;
        }
        line += len + (line[len] == '\n');
    }
    int count = allowed_count;
    os_unfair_lock_unlock(&allowed_lock);
    return count;
}

int zm_inventory_allowed_left(void) {
    os_unfair_lock_lock(&allowed_lock);
    int count = allowed_count;
    os_unfair_lock_unlock(&allowed_lock);
    return count;
}

// whether CheatsOk(ent), returning to return_address, is for one of the commands zm_inventory_allow_commands let
// through, and takes it if so: the give or take command asks (not noclip, god or the other cheat commands, which a
// player could type meanwhile), with that name, for the local player. One the game runs while the player isn't alive
// is used up too, and refused, as the game's check refuses it; *held gets how long after it was allowed it ran.
static bool let_through(void *ent, uint64_t return_address, long long *held) {
    bool give = return_address == base + ADDR_CheatsOk_From_Give;
    if ((!give && return_address != base + ADDR_CheatsOk_From_Take) || ent == NULL ||
        (uint64_t)ent != entities() || SV_Cmd_ArgvBuffer == NULL)
        return false;
    char name[sizeof(allowed[0].name)];
    name[0] = '\0';
    SV_Cmd_ArgvBuffer(1, name, sizeof(name));
    bool found = false;
    os_unfair_lock_lock(&allowed_lock);
    for (int i = 0; i < allowed_count && !found; i++) {
        if (allowed[i].give == give && strcasecmp(allowed[i].name, name) == 0) {
            allowed[i] = allowed[--allowed_count];
            found = true;
        }
    }
    if (held != NULL)
        *held = now_ms() - allowed_at;
    os_unfair_lock_unlock(&allowed_lock);
    int health = 0;
    return found && safe_read((uint64_t)ent + gentity_Health, &health, sizeof(health)) && health > 0;
}

// ---- the full loadout (zm_loadout.c): recorded from the player state and the server script VM's variables, and
// restored on the server thread, from the give command, through the game's own functions

#define Script_Server 0
#define Aae_PapLevelUp 0xe843ceb2 // tfoption.gsc's repack function, known only by its name's hash
#define WeaponInventory_AltMode 3 // WeaponDef's inventory type: a gun's alternate mode (its ammo is the gun's)
#define WeaponInventory_DualWieldLeft 5 // and a dual-wield gun's left hand
#define Perk_None 0x69     // what BG_GetPerkIndexForName gives for a name that isn't a perk
#define Attachment_Count 44
#define Restore_Ms 30000   // how long a restore set up from Lua waits for its give command
#define Restore_Command "bo3macfix_restore"
#define Switch_Command "bo3macfix_switch" // the gun in hand again, after a restore during a map's intro
#define Switch_Ms 10000                   // how long its command is waited for
#define Shield_Command "bo3macfix_shield" // the shield's health, once the shield given by the restore is held
#define Shield_Wait_Ms 30000              // how long after the restore the shield is waited for
#define Shield_Resend_Ms 3000             // and its command, before it's sent again
#define Aae_ShieldHud 0x70c029fa          // riotshield::?70c029fa(health, full): the HUD's zmInventory.shield_health

// the game's handle for a script function: its "script::function" name, then where its code starts
typedef struct {
    char name[0x108];
    const void *code_pos;
} script_handle_t;
_Static_assert(sizeof(script_handle_t) == 0x110, "the game's script function handle is 0x110 bytes");

static int (*G_GivePlayerWeapon)(void *ent, void *client, uint64_t weapon, uint64_t options, int unused, int acvi);
static void (*G_GiveStartAmmo)(void *ent, uint64_t weapon, int held_before);
static int (*G_TakePlayerWeapon)(void *ent, void *client, uint64_t weapon, bool flag);
static void (*G_SelectWeapon)(int client_num, uint64_t weapon);
static void (*G_SwitchToWeapon)(void *ent, int immediate, uint64_t weapon);
static uint64_t (*BG_GetWeaponForName)(const char *name);
static int (*BG_GetPerkIndexForName)(const char *name);
static bool (*BG_HasPerk)(void *client, int index);
static script_handle_t *(*Scr_GetFunctionHandle)(script_handle_t *out, int inst, const char *script, uint32_t ns,
                                                 uint32_t name, int *checksum, bool required);
static unsigned int (*Scr_ExecEntThread)(void *ent, int params, script_handle_t handle);
static void (*Scr_FreeThread)(int inst, unsigned int thread);
static void (*Scr_AddString)(int inst, const char *text);
static void (*Scr_AddWeapon)(int inst, uint64_t weapon, int client_num);
static void (*Scr_AddInt)(int inst, int value);
static int (*BG_GetAmmoInClip)(void *ps, uint64_t weapon);
static int (*BG_GetAmmoStock)(void *ps, uint64_t weapon);
static void (*BG_SetAmmoInClip)(void *ps, uint64_t weapon, int amount);
static void (*BG_SetAmmoStock)(void *ps, uint64_t weapon, int amount);
static int (*BG_GetClipSize)(uint64_t weapon);
static int (*BG_WeaponIsClipOnly)(uint64_t weapon);
static int (*BG_GetMaxAmmo)(void *ps, uint64_t weapon, uint64_t arg);
static void *(*Dvar_FindByHash)(uint32_t hash);
static int (*Dvar_GetInt)(const void *dvar);

static void (*G_SetShieldHealth)(void *ent, int health);
static int (*BG_GetShieldHealth)(int unused, int client_num, uint64_t weapon);
static uint64_t *(*BG_ThreadBgs)(void);
static uint64_t (*Gadget_SlotWeapon)(void *client, int slot);
static void (*G_GadgetPowerSet)(void *client, void *gadget, uint64_t weapon, int time, float power);

// the server script VM, read through safe_read
static loadout_vm_t server_vm(void) {
    return (loadout_vm_t){ safe_read, base + ADDR_scrVarGlob + Script_Server * 0x100,
                           base + ADDR_scrClassMap + Script_Server * sizeof(uint64_t), base + ADDR_scrStringTable,
                           base + ADDR_scrVarPub + Script_Server * 0x78 + scrVarPub_LevelId };
}

// the game's attachment names, by the ids a Weapon's attachment fields hold; NULL where one can't be read
static void attachment_names(char text[Attachment_Count][32], const char *names[Attachment_Count]) {
    uint64_t table[Attachment_Count];
    bool have = safe_read(base + ADDR_bg_attachmentNames, table, sizeof(table));
    for (int i = 0; i < Attachment_Count; i++) {
        names[i] = NULL;
        size_t len = 0;
        for (; have && len + 1 < sizeof(text[i]); len++) {
            if (!safe_read(table[i] + len, &text[i][len], 1))
                break;
            if (text[i][len] == '\0') {
                names[i] = text[i];
                break;
            }
        }
    }
}

// the held weapon entry i
static bool held_weapon(uint64_t client, int i, uint64_t *weapon, uint64_t *options, uint32_t *acvi) {
    uint64_t entry = client + gclient_HeldWeapons + (uint64_t)i * gclient_HeldWeapon_Size;
    return safe_read(entry, weapon, sizeof(*weapon)) &&
           (options == NULL || safe_read(entry + gclient_HeldWeapon_Options, options, sizeof(*options))) &&
           (acvi == NULL || safe_read(entry + gclient_HeldWeapon_Acvi, acvi, sizeof(*acvi)));
}

// whether the player holds weapon
static bool holds(uint64_t client, uint64_t weapon) {
    for (int i = 0; i < gclient_HeldWeapon_Count; i++) {
        uint64_t held = 0;
        if (held_weapon(client, i, &held, NULL, NULL) && held == weapon)
            return true;
    }
    return false;
}

// a held weapon's clip and stock, as GetWeaponAmmoClip and GetWeaponAmmoStock read them (read-only game functions,
// called once the weapon and the player state have read back)
static bool read_ammo(uint64_t client, uint64_t weapon, int *clip, int *stock) {
    if (BG_GetAmmoInClip == NULL || BG_GetAmmoStock == NULL || BG_WeaponIsClipOnly == NULL || !server_running() ||
        !holds(client, weapon))
        return false;
    *clip = BG_GetAmmoInClip((void *)client, weapon);
    *stock = BG_WeaponIsClipOnly(weapon) ? 0 : BG_GetAmmoStock((void *)client, weapon);
    return *clip >= 0 && *stock >= 0;
}

// a held weapon that isn't a gun (a grenade, a mine, the shield, a hero weapon, the melee weapon), by the name the
// server looks it up by, with its count; not the ones a gun has (its alternate mode, its left hand)
static void read_item(uint64_t client, uint64_t weapon, const char *variant, int type, const char *const *names,
                      loadout_t *out) {
    char name[72];
    if (type == WeaponInventory_AltMode || type == WeaponInventory_DualWieldLeft || out->item_count >= Loadout_Max_Items)
        return;
    if (!weapon_script_name(weapon, name, sizeof(name)))
        snprintf(name, sizeof(name), "%s", variant);
    loadout_item_t *item = &out->items[out->item_count];
    if (!loadout_weapon_name(name, weapon, names, Attachment_Count, item->name, sizeof(item->name)) ||
        !loadout_item_worth_saving(item->name) || (type == 0 && !loadout_primary_is_item(item->name)))
        return;
    for (int i = 0; i < out->item_count; i++)
        if (strcmp(out->items[i].name, item->name) == 0)
            return;
    item->have_ammo = read_ammo(client, weapon, &item->clip, &item->stock);
    out->item_count++;
}

// a weapon's WeaponDef, as BG_GetWeaponDef (0x12362cd) finds it
static bool weapon_def(uint64_t weapon, uint64_t *def) {
    uint32_t index = (uint32_t)(weapon & Weapon_Index_Mask);
    int32_t last = 0;
    uint64_t variant = 0;
    return index != 0 && safe_read(base + ADDR_bg_lastWeaponIndex, &last, sizeof(last)) && index <= (uint32_t)last &&
           safe_read(base + ADDR_bg_weaponVariantDefs + index * sizeof(uint64_t), &variant, sizeof(variant)) &&
           safe_read(variant + WeaponVariantDef_WeaponDef, def, sizeof(*def)) && *def != 0;
}

// the first held shield (the one the engine's shield functions take) and its full health
static uint64_t held_shield(uint64_t client, int *full) {
    for (int i = 0; i < gclient_HeldWeapon_Count; i++) {
        uint64_t weapon = 0, def = 0;
        int32_t type = 0;
        if (held_weapon(client, i, &weapon, NULL, NULL) && (weapon & Weapon_Index_Mask) != 0 && weapon_def(weapon, &def) &&
            safe_read(def + WeaponDef_Type, &type, sizeof(type)) && type == WeaponType_Shield)
            return safe_read(def + WeaponDef_StartHitPoints, full, sizeof(*full)) && *full > 0 &&
                   *full <= Loadout_Max_Shield_Health ? weapon : 0;
    }
    return 0;
}

// the shield's health where the server keeps it (level_bgs, for the player's client number)
static bool stored_shield_health(uint64_t client, int *health) {
    int32_t client_num = -1;
    return safe_read(client + gclient_ClientNum, &client_num, sizeof(client_num)) && client_num >= 0 &&
           client_num < 64 &&
           safe_read(base + ADDR_level_bgs + bgs_ShieldHealth + (uint64_t)client_num * bgs_Client_Size, health,
                     sizeof(*health));
}

// the held shield's health (read-only), if Items has the shield
static void read_shield_health(uint64_t client, loadout_t *out) {
    int full = 0, health = -1;
    char name[72];
    uint64_t weapon = held_shield(client, &full);
    if (weapon == 0 || !weapon_script_name(weapon, name, sizeof(name)) || !stored_shield_health(client, &health) ||
        health < 0 || health > full)
        return;
    for (int i = 0; i < out->item_count; i++) {
        const char *item = out->items[i].name;
        size_t len = strlen(name);
        if (loadout_is_shield(item) && strncmp(item, name, len) == 0 && (item[len] == '\0' || item[len] == '+')) {
            snprintf(out->shield_name, sizeof(out->shield_name), "%s", item);
            out->shield_health = health;
            out->have_shield_health = true;
            return;
        }
    }
}

bool zm_inventory_read_loadout(loadout_t *out, char *note, size_t note_size) {
    loadout_clear(out);
    if (note_size > 0)
        note[0] = '\0';
    if (!server_running())
        return false;
    uint64_t first = entities(), client = 0, current = 0;
    if (first == 0 || !safe_read(first + gentity_Client, &client, sizeof(client)) || client == 0 ||
        !safe_read(client + gclient_Weapon, &current, sizeof(current)))
        return false;
    char text[Attachment_Count][32];
    const char *names[Attachment_Count];
    attachment_names(text, names);
    // the ammo types and perks are the zombies scripts' own (self.aat, self.perks_active)
    loadout_vm_t vm = server_vm();
    uint32_t player = loadout_vm_entity(&vm, 0);
    loadout_aat_t aats[16];
    int aat_count = player != 0 ? loadout_vm_read_aats(&vm, player, aats, 16) : -1;
    for (int i = 0; i < gclient_HeldWeapon_Count && out->gun_count < Loadout_Max_Guns; i++) {
        uint64_t weapon = 0, options = 0;
        uint32_t acvi = 0;
        char variant[72];
        int type = -1;
        if (!held_weapon(client, i, &weapon, &options, &acvi) || (weapon & Weapon_Index_Mask) == 0 ||
            !weapon_info(weapon, variant, sizeof(variant), &type))
            continue;
        if (type != 0 || loadout_primary_is_item(variant)) {
            read_item(client, weapon, variant, type, names, out);
            continue;
        }
        // by the name the server looks it up by, else the variant's own
        char script_name[72];
        if (weapon_script_name(weapon, script_name, sizeof(script_name)))
            snprintf(variant, sizeof(variant), "%s", script_name);
        loadout_gun_t *gun = &out->guns[out->gun_count];
        if (!loadout_weapon_name(variant, weapon, names, Attachment_Count, gun->name, sizeof(gun->name)))
            continue;
        gun->options = options;
        gun->acvi = acvi;
        gun->have_ammo = read_ammo(client, weapon, &gun->clip, &gun->stock);
        for (int a = 0; a < aat_count; a++)
            if (aats[a].weapon == weapon)
                snprintf(gun->aat, sizeof(gun->aat), "%s", aats[a].aat);
        if (weapon == current)
            out->held = out->gun_count;
        out->gun_count++;
    }
    // the hero weapon's charge: gadget slot 0's power, which the stock _zm_hero_weapon.gsc reads for the HUD
    for (int i = 0; i < out->item_count && !out->have_hero_power; i++) {
        float power = 0;
        if (loadout_is_hero_weapon(out->items[i].name) &&
            safe_read(client + gclient_Gadget0Power, &power, sizeof(power)) && power >= 0 && power <= 100.5f) {
            snprintf(out->hero_name, sizeof(out->hero_name), "%s", out->items[i].name);
            out->hero_power = power > 100 ? 100 : (int)(power + 0.5f);
            out->have_hero_power = true;
        }
    }
    read_shield_health(client, out);
    // AAE's Repack-a-Punch levels (level.pap_lvup, keyed by self.guid and the root weapon's name)
    char guid[32];
    bool have_guid = player != 0 && loadout_vm_guid(&vm, player, guid, sizeof(guid));
    int pap_count = loadout_vm_read_pap_levels(&vm, have_guid ? guid : NULL, out);
    uint32_t perks = player != 0 ? loadout_vm_array_field(&vm, player, "perks_active") : 0;
    int perk_count = perks != 0 ? loadout_vm_read_strings(&vm, perks, out->perks, Loadout_Max_Perks) : -1;
    out->perk_count = perk_count > 0 ? perk_count : 0;
    if (note_size > 0)
        snprintf(note, note_size, "%s%s", player == 0 ? "no script object for the player; " : "",
                 aat_count < 0 ? "self.aat unreadable; " : "");
    if (note_size > 0 && player != 0 && perks == 0)
        snprintf(note + strlen(note), note_size - strlen(note), "no self.perks_active; ");
    if (note_size > 0 && pap_count > 0 && !have_guid)
        snprintf(note + strlen(note), note_size - strlen(note), "repack levels matched without self.guid; ");
    if (getenv("BO3MACFIX_CHECKPOINT_PAP_DEBUG") != NULL && player != 0) {
        char text[512];
        loadout_vm_describe_pap(&vm, player, text, sizeof(text));
        printf("BO3MacFix: zm inventory: repack levels: %d read (guid %s); %s\n", pap_count, have_guid ? guid : "none",
               text);
    }
    return true;
}

// ---- restoring, on the server thread

typedef struct {
    uint8_t *ent, *client;
    int entnum;
    uint64_t switched_to; // the gun the restore put in hand
} restore_context_t;

// whether the player has control: not linked to another entity (Ascension's intro links it to the lander), its
// controls not frozen, its weapons not off; what it lacks in why, and the raw values in raw (for the log)
static bool player_controls(uint64_t ent, uint64_t client, char *why, size_t why_size, char *raw, size_t raw_size) {
    uint64_t tag_info = 0;
    uint8_t link = 0, weapon_flags = 0;
    uint32_t flags = 0;
    bool read = safe_read(ent + gentity_TagInfo, &tag_info, sizeof(tag_info)) &&
                safe_read(ent + gentity_LinkFlags, &link, sizeof(link)) &&
                safe_read(client + gclient_Flags, &flags, sizeof(flags)) &&
                safe_read(client + gclient_WeaponFlags, &weapon_flags, sizeof(weapon_flags));
    if (raw_size > 0)
        snprintf(raw, raw_size, "tag info %s, link byte %#x, flags %#x, weapon flags %#x", tag_info != 0 ? "set" : "none",
                 link, flags, weapon_flags);
    if (!read) {
        snprintf(why, why_size, "unreadable");
        return false;
    }
    snprintf(why, why_size, "%s%s%s", tag_info != 0 ? "linked " : "", (flags & gclient_Frozen) ? "frozen " : "",
             (weapon_flags & gclient_WeaponsDisabled) ? "weapons off " : "");
    size_t len = strlen(why);
    if (len > 0)
        why[len - 1] = '\0';
    return len == 0;
}

// the weapon by the name the server looks it up by, or else by its other form (with or without "_zm")
static uint64_t op_weapon_for_name(void *context, const char *name) {
    uint64_t weapon = BG_GetWeaponForName(name);
    char other[Loadout_Name_Size];
    if ((weapon & Weapon_Index_Mask) == 0 && loadout_other_name(name, other, sizeof(other)))
        weapon = BG_GetWeaponForName(other);
    return (weapon & Weapon_Index_Mask) != 0 ? weapon : 0;
}

static int op_primaries(void *context, uint64_t *out, int max) {
    restore_context_t *c = context;
    int count = 0;
    for (int i = 0; i < gclient_HeldWeapon_Count && count < max; i++) {
        uint64_t weapon = 0;
        char name[72];
        int type = -1;
        // a shield or hero weapon is an item (Items), not a gun to take
        if (held_weapon((uint64_t)c->client, i, &weapon, NULL, NULL) && (weapon & Weapon_Index_Mask) != 0 &&
            weapon_info(weapon, name, sizeof(name), &type) && type == 0 && !loadout_primary_is_item(name))
            out[count++] = weapon;
    }
    return count;
}

// what the take command does (Cmd_Take_f)
static bool op_take(void *context, uint64_t weapon) {
    restore_context_t *c = context;
    uint64_t *in_hand = (uint64_t *)(c->client + gclient_Weapon);
    if (*in_hand == weapon) {
        *in_hand = 0;
        *(uint64_t *)(c->client + gclient_WeaponSwitch) = 0;
        G_SelectWeapon(c->entnum, 0);
    }
    G_TakePlayerWeapon(c->ent, c->client, weapon, false);
    return !holds((uint64_t)c->client, weapon);
}

// what the GiveWeapon builtin does, then GiveStartAmmo (which the Pack-a-Punch machine calls after it)
static bool op_give(void *context, uint64_t weapon, uint64_t options, uint32_t acvi) {
    restore_context_t *c = context;
    if (G_GivePlayerWeapon(c->ent, c->client, weapon, options, 0, (int)acvi) == 0)
        return false;
    G_GiveStartAmmo(c->ent, weapon, 0);
    return holds((uint64_t)c->client, weapon);
}

// a server script function by its namespace's and name's hashes, false if the scripts loaded don't have it
static bool script_function_hashed(script_handle_t *handle, const char *script, uint32_t ns, uint32_t name) {
    memset(handle, 0, sizeof(*handle));
    Scr_GetFunctionHandle(handle, Script_Server, script, ns, name, NULL, false);
    const void *missing = NULL;
    safe_read(base + ADDR_scrDefaultFunction, &missing, sizeof(missing));
    return handle->code_pos != NULL && handle->code_pos != missing;
}

// a server script function, false if the scripts loaded don't have it (the game hands out a do-nothing one then)
static bool script_function(script_handle_t *handle, const char *script, const char *ns, const char *name) {
    memset(handle, 0, sizeof(*handle));
    Scr_GetFunctionHandle(handle, Script_Server, script, loadout_script_hash(ns), loadout_script_hash(name), NULL,
                          false);
    const void *missing = NULL;
    safe_read(base + ADDR_scrDefaultFunction, &missing, sizeof(missing));
    return handle->code_pos != NULL && handle->code_pos != missing;
}

// aat::acquire(weapon, aat) on the player, as the Pack-a-Punch machine calls it; true if self.aat has it then
static bool op_acquire_aat(void *context, uint64_t weapon, const char *aat) {
    restore_context_t *c = context;
    script_handle_t handle;
    if (!script_function(&handle, "scripts/shared/aat_shared", "aat", "acquire"))
        return false;
    int client_num = 0;
    safe_read(base + ADDR_scrClientNum, &client_num, sizeof(client_num));
    // the last parameter first
    Scr_AddString(Script_Server, aat);
    Scr_AddWeapon(Script_Server, weapon, client_num);
    Scr_FreeThread(Script_Server, Scr_ExecEntThread(c->ent, 2, handle));
    loadout_vm_t vm = server_vm();
    loadout_aat_t aats[16];
    int count = loadout_vm_read_aats(&vm, loadout_vm_entity(&vm, c->entnum), aats, 16);
    for (int i = 0; i < count; i++)
        if (aats[i].weapon == weapon && strcmp(aats[i].aat, aat) == 0)
            return true;
    return false;
}

static bool op_has_perk(void *context, const char *perk) {
    restore_context_t *c = context;
    int index = BG_GetPerkIndexForName(perk);
    return index != Perk_None && BG_HasPerk(c->client, index);
}

// zm_perks::give_perk(perk, false) on the player, as the Perkaholic gum gives each perk; true if it has the perk then
static bool op_give_perk(void *context, const char *perk) {
    restore_context_t *c = context;
    script_handle_t handle;
    if (BG_GetPerkIndexForName(perk) == Perk_None ||
        !script_function(&handle, "scripts/zm/_zm_perks", "zm_perks", "give_perk"))
        return false;
    Scr_AddInt(Script_Server, 0);
    Scr_AddString(Script_Server, perk);
    Scr_FreeThread(Script_Server, Scr_ExecEntThread(c->ent, 2, handle));
    return op_has_perk(context, perk);
}

// the repack level a gun has now (level.pap_lvup), 0 for none
static int pap_level_now(int entnum, const char *name) {
    loadout_vm_t vm = server_vm();
    loadout_t one;
    loadout_clear(&one);
    snprintf(one.guns[0].name, sizeof(one.guns[0].name), "%s", name);
    one.gun_count = 1;
    char guid[32];
    uint32_t player = loadout_vm_entity(&vm, entnum);
    bool have_guid = player != 0 && loadout_vm_guid(&vm, player, guid, sizeof(guid));
    return loadout_vm_read_pap_levels(&vm, have_guid ? guid : NULL, &one) > 0 ? one.guns[0].pap_level : 0;
}

// AAE's repack level through its own function, tfoption.gsc's ?e843ceb2(weapon) on the player (the one its
// Pack-a-Punch thread calls): it sets an undefined level to 1 and adds 1 to one under 5, or makes it 5 at once with
// its Easter egg reward (self.?def06a03 >= 90 and tfoption_eereward). Called until the level reads back at or above
// the saved one, and no more once a call doesn't raise it. A level above the saved one is the reward's, so it stays.
static bool op_set_pap_level(void *context, uint64_t weapon, const char *name, int level) {
    restore_context_t *c = context;
    script_handle_t handle;
    // not with AAE's Repack-a-Punch option off now (a dvar it has once its options are read; a game without it
    // has no such function either)
    void *option = Dvar_FindByHash != NULL ? Dvar_FindByHash(loadout_script_hash("tfoption_dpap")) : NULL;
    if (option != NULL && Dvar_GetInt(option) != 1)
        return false;
    if (!script_function_hashed(&handle, "scripts/zm/tfoption", loadout_script_hash("tfoption"), Aae_PapLevelUp))
        return false;
    int client_num = 0;
    safe_read(base + ADDR_scrClientNum, &client_num, sizeof(client_num));
    int now = pap_level_now(c->entnum, name);
    for (int calls = 0; now < level && calls < Loadout_Max_Pap_Level; calls++) {
        Scr_AddWeapon(Script_Server, weapon, client_num);
        Scr_FreeThread(Script_Server, Scr_ExecEntThread(c->ent, 1, handle));
        int after = pap_level_now(c->entnum, name);
        if (after <= now)
            return false;
        now = after;
    }
    return now >= level;
}

static bool op_holds(void *context, uint64_t weapon) {
    restore_context_t *c = context;
    return holds((uint64_t)c->client, weapon);
}

// zm_weapons::weapon_give(weapon, is_upgrade 0, magic_box 0, nosound 1, b_switch_weapon 0): the zombies scripts' own
// give, which puts a grenade, mine, hero or melee weapon in its slot (zm_utility's set_player_*, the old one taken)
// and gives equipment like the shield through zm_equipment::give. True once the give ran: equipment is held only a
// moment later (live: Shadows of Evil's shield wasn't held yet right after, and was by the next save).
static bool op_give_item(void *context, uint64_t weapon) {
    restore_context_t *c = context;
    script_handle_t handle;
    if (!script_function(&handle, "scripts/zm/_zm_weapons", "zm_weapons", "weapon_give"))
        return false;
    int client_num = 0;
    safe_read(base + ADDR_scrClientNum, &client_num, sizeof(client_num));
    // the last parameter first
    Scr_AddInt(Script_Server, 0);
    Scr_AddInt(Script_Server, 1);
    Scr_AddInt(Script_Server, 0);
    Scr_AddInt(Script_Server, 0);
    Scr_AddWeapon(Script_Server, weapon, client_num);
    Scr_FreeThread(Script_Server, Scr_ExecEntThread(c->ent, 5, handle));
    return true;
}

// what SetWeaponAmmoClip and SetWeaponAmmoStock do: clamped to the clip size and the most stock (a clip-only
// weapon, a grenade say, has only the clip)
static bool op_set_ammo(void *context, uint64_t weapon, int clip, int stock) {
    restore_context_t *c = context;
    if (!holds((uint64_t)c->client, weapon))
        return false;
    int size = BG_GetClipSize(weapon);
    BG_SetAmmoInClip(c->client, weapon, clip < 0 ? 0 : clip > size ? size : clip);
    if (!BG_WeaponIsClipOnly(weapon)) {
        uint64_t arg = 0;
        safe_read(base + ADDR_bg_maxAmmoArg, &arg, sizeof(arg));
        int most = BG_GetMaxAmmo(c->client, weapon, arg);
        BG_SetAmmoStock(c->client, weapon, stock < 0 ? 0 : stock > most ? most : stock);
    }
    return true;
}

// what the GadgetPowerSet builtin does for slot 0 (the stock hero weapon script's), if the hero weapon is in that slot;
// true if the power reads back as given
static bool op_set_hero_power(void *context, uint64_t weapon, int power) {
    restore_context_t *c = context;
    if (Gadget_SlotWeapon(c->client, 0) != weapon)
        return false;
    int time = 0;
    safe_read(base + ADDR_level_Time, &time, sizeof(time));
    G_GadgetPowerSet(c->client, c->client + gclient_Gadgets, weapon, time, (float)power);
    float now = -1;
    return safe_read((uint64_t)c->client + gclient_Gadget0Power, &now, sizeof(now)) && now > power - 1 && now < power + 1;
}

// the shield's health waits for the shield: the scripts' give (zm_equipment::give) lands a moment after the restore,
// and puts it at full. The switch poll (UI thread) sends Shield_Command once the shield is held; run_shield sets it.
static os_unfair_lock shield_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t shield_weapon; // 0: nothing waiting
static int shield_health, shield_held_polls;
static long long shield_until, shield_sent_at, shield_command_until;

// a new map or match: the restore that set the health up was the last game's, and a shield held now (within the 30 s)
// isn't the one it gave
static void drop_shield_health(const char *why) {
    os_unfair_lock_lock(&shield_lock);
    bool waiting = shield_weapon != 0;
    shield_weapon = 0;
    shield_held_polls = 0;
    shield_until = shield_sent_at = shield_command_until = 0;
    os_unfair_lock_unlock(&shield_lock);
    if (waiting)
        printf("BO3MacFix: zm inventory: %s: the shield's health from the last restore isn't set any more\n", why);
}

void zm_inventory_new_map(void) {
    drop_shield_health("a new map");
}

static bool op_set_shield_health(void *context, uint64_t weapon, int health) {
    (void)context;
    if (G_SetShieldHealth == NULL || health < 0)
        return false;
    os_unfair_lock_lock(&shield_lock);
    shield_weapon = weapon;
    shield_health = health;
    shield_held_polls = 0;
    shield_until = now_ms() + Shield_Wait_Ms;
    shield_sent_at = shield_command_until = 0;
    os_unfair_lock_unlock(&shield_lock);
    return true;
}

static void op_switch_to(void *context, uint64_t weapon) {
    restore_context_t *c = context;
    G_SwitchToWeapon(c->ent, 0, weapon);
    c->switched_to = weapon;
}

static os_unfair_lock restore_lock = OS_UNFAIR_LOCK_INIT;
static loadout_t restore_plan;
static bool restore_pap_test; // the plan is the test switch's, given the Pack-a-Punch machine's way
static long long restore_until; // when the plan set up stops waiting for its command, 0 for none
static char restore_result[512];
static bool restore_finished;
// after a restore made while the player had no control (loadout_switch_poll), and the switch its command makes
static loadout_switch_t switch_watch;
static uint64_t switch_weapon;
static long long switch_until;
static char switch_state[64]; // what the last poll saw, logged when it changes

static const char *set_restore(const loadout_t *plan, bool pap_test) {
    os_unfair_lock_lock(&restore_lock);
    restore_plan = *plan;
    restore_pap_test = pap_test;
    restore_until = now_ms() + Restore_Ms;
    restore_finished = false;
    restore_result[0] = '\0';
    os_unfair_lock_unlock(&restore_lock);
    return "give " Restore_Command;
}

const char *zm_inventory_set_restore(const loadout_t *plan) {
    return set_restore(plan, false);
}

const char *zm_inventory_set_pap_test(const loadout_t *plan) {
    return set_restore(plan, true);
}

// the held weapon with weapon's index, whatever its attachments, 0 for none
static uint64_t held_like(uint64_t client, uint64_t weapon) {
    for (int i = 0; i < gclient_HeldWeapon_Count; i++) {
        uint64_t held = 0;
        if (held_weapon(client, i, &held, NULL, NULL) && (held & Weapon_Index_Mask) == (weapon & Weapon_Index_Mask))
            return held;
    }
    return 0;
}

// for the test switch: each gun given as the Pack-a-Punch machine gives one when there's room for it
// (zm_weapons::give_build_kit_weapon, which picks the camo and the build kit's attachments, then GiveStartAmmo), then
// aat::acquire(weapon, aat) and the perks, so a test game has guns the game itself upgraded
static void run_pap_test(restore_context_t *c, const loadout_t *plan, char *out, size_t out_size) {
    script_handle_t give;
    bool have = script_function(&give, "scripts/zm/_zm_weapons", "zm_weapons", "give_build_kit_weapon");
    int client_num = 0, given = 0, aats = 0, perks = 0;
    safe_read(base + ADDR_scrClientNum, &client_num, sizeof(client_num));
    uint64_t first = 0;
    for (int i = 0; have && i < plan->gun_count; i++) {
        uint64_t weapon = op_weapon_for_name(c, plan->guns[i].name);
        if (weapon == 0)
            continue;
        Scr_AddWeapon(Script_Server, weapon, client_num);
        Scr_FreeThread(Script_Server, Scr_ExecEntThread(c->ent, 1, give));
        uint64_t held = held_like((uint64_t)c->client, weapon);
        if (held == 0)
            continue;
        G_GiveStartAmmo(c->ent, held, 0);
        given++;
        if (first == 0)
            first = held;
        if (plan->guns[i].aat[0] != '\0' && op_acquire_aat(c, held, plan->guns[i].aat))
            aats++;
    }
    for (int i = 0; i < plan->perk_count; i++)
        if (!op_has_perk(c, plan->perks[i]) && op_give_perk(c, plan->perks[i]))
            perks++;
    if (first != 0)
        op_switch_to(c, first);
    snprintf(out, out_size, "Pack-a-Punch test: gave %d of %d guns%s, %d ammo types, %d perks", given, plan->gun_count,
             have ? "" : " (no zm_weapons::give_build_kit_weapon)", aats, perks);
}

bool zm_inventory_restore_result(char *out, size_t out_size) {
    os_unfair_lock_lock(&restore_lock);
    bool finished = restore_finished;
    if (finished && out_size > 0)
        snprintf(out, out_size, "%s", restore_result);
    restore_finished = false;
    os_unfair_lock_unlock(&restore_lock);
    return finished;
}

static void finish_restore(const char *text) {
    printf("BO3MacFix: zm inventory: restoring the saved loadout: %s\n", text);
    os_unfair_lock_lock(&restore_lock);
    snprintf(restore_result, sizeof(restore_result), "%s", text);
    restore_finished = true;
    os_unfair_lock_unlock(&restore_lock);
}

// which of BO3MacFix's give commands the client command CheatsOk was asked for (returning to return_address) is:
// 1 "give bo3macfix_restore", 2 "give bo3macfix_switch", 3 "give bo3macfix_shield", 0 none
static int restore_command(uint64_t return_address) {
    if (return_address != base + ADDR_CheatsOk_From_Give || SV_Cmd_ArgvBuffer == NULL)
        return 0;
    char arg[64];
    arg[0] = '\0';
    SV_Cmd_ArgvBuffer(1, arg, sizeof(arg));
    return strcmp(arg, Restore_Command) == 0 ? 1 : strcmp(arg, Switch_Command) == 0 ? 2 :
           strcmp(arg, Shield_Command) == 0  ? 3 : 0;
}

// the restore set up last, for the local player, while it's alive; in the give command, on the server thread, where
// the game itself runs script callbacks
static void run_restore(void *ent) {
    os_unfair_lock_lock(&restore_lock);
    bool set_up = restore_until != 0, pending = set_up && now_ms() < restore_until, pap_test = restore_pap_test;
    static loadout_t plan;
    plan = restore_plan;
    restore_until = 0;
    os_unfair_lock_unlock(&restore_lock);
    uint64_t first = entities(), client = 0;
    int health = 0;
    // A command with nothing set up (Lua sent it again while the game held the first back, and the first has run
    // since, often in the same frame) keeps the result Lua hasn't read: "nothing set up" would have it set the restore
    // up and send it once more, and restore again over what the player has by then.
    if (!set_up) {
        printf("BO3MacFix: zm inventory: a restore command with nothing set up (one sent again), ignored\n");
        return;
    }
    if (!pending)
        return finish_restore("nothing set up (or set up too long ago)");
    // the local player, the first entity in a game hosted here, as for the give and take commands let through
    if (ent == NULL || first == 0 || (uint64_t)ent != first ||
        !safe_read((uint64_t)ent + gentity_Client, &client, sizeof(client)) || client == 0 ||
        !safe_read((uint64_t)ent + gentity_Health, &health, sizeof(health)) || health <= 0)
        return finish_restore("not for the local player, or it isn't alive");
    restore_context_t context = { ent, (uint8_t *)client, 0, 0 };
    char text[512];
    if (pap_test) {
        run_pap_test(&context, &plan, text, sizeof(text));
        return finish_restore(text);
    }
    loadout_ops_t ops = { &context,     op_weapon_for_name, op_primaries, op_take,      op_give,
                          op_acquire_aat, op_has_perk,      op_give_perk, op_switch_to, op_set_pap_level,
                          op_holds,       op_give_item,     op_set_ammo,  op_set_hero_power, op_set_shield_health };
    loadout_result_t result;
    loadout_restore(&plan, &ops, &result);
    loadout_result_text(&result, text, sizeof(text));
    // a map's intro that still has the player (linked, frozen) can put a gun of its own in hand as it ends
    char why[64], raw[96];
    bool controlled = player_controls((uint64_t)ent, client, why, sizeof(why), raw, sizeof(raw));
    os_unfair_lock_lock(&restore_lock);
    loadout_switch_start(&switch_watch, context.switched_to, controlled);
    switch_weapon = 0;
    switch_state[0] = '\0'; // the first poll is logged
    os_unfair_lock_unlock(&restore_lock);
    if (context.switched_to != 0 && !controlled)
        printf("BO3MacFix: zm inventory: the player has no control yet (%s; %s): the gun in hand is looked at again "
               "once it has\n", why, raw);
    finish_restore(text);
}

// "give bo3macfix_switch": the gun the restore put in hand, again, if the player still holds it and has another in hand
static void run_switch(void *ent) {
    os_unfair_lock_lock(&restore_lock);
    uint64_t weapon = now_ms() < switch_until ? switch_weapon : 0;
    switch_weapon = 0;
    os_unfair_lock_unlock(&restore_lock);
    uint64_t first = entities(), client = 0, in_hand = 0;
    int health = 0;
    if (weapon == 0 || ent == NULL || (uint64_t)ent != first ||
        !safe_read((uint64_t)ent + gentity_Client, &client, sizeof(client)) || client == 0 ||
        !safe_read((uint64_t)ent + gentity_Health, &health, sizeof(health)) || health <= 0 ||
        !safe_read(client + gclient_Weapon, &in_hand, sizeof(in_hand))) {
        printf("BO3MacFix: zm inventory: a switch command with nothing set up, or not for the local player alive\n");
        return;
    }
    char name[72] = "?";
    weapon_script_name(weapon, name, sizeof(name));
    if (in_hand == weapon || !holds(client, weapon)) {
        printf("BO3MacFix: zm inventory: no switch to %s: %s\n", name, in_hand == weapon ? "in hand already" : "not held");
        return;
    }
    G_SwitchToWeapon(ent, 0, weapon);
    printf("BO3MacFix: zm inventory: switched to the saved gun in hand again: %s\n", name);
}

// "give bo3macfix_shield": the saved health on the shield the restore gave, through the engine's own setter (what the
// player's weaponhealth field does), then the HUD's meter the way the shield's damage function sets it
static void run_shield(void *ent) {
    os_unfair_lock_lock(&shield_lock);
    uint64_t weapon = now_ms() < shield_command_until ? shield_weapon : 0;
    int health = shield_health;
    os_unfair_lock_unlock(&shield_lock);
    uint64_t first = entities(), client = 0;
    int alive = 0, full = 0;
    if (weapon == 0 || ent == NULL || (uint64_t)ent != first ||
        !safe_read((uint64_t)ent + gentity_Client, &client, sizeof(client)) || client == 0 ||
        !safe_read((uint64_t)ent + gentity_Health, &alive, sizeof(alive)) || alive <= 0) {
        printf("BO3MacFix: zm inventory: a shield command with nothing set up, or not for the local player alive\n");
        return;
    }
    // (not held: the poll sends it again)
    if (held_shield(client, &full) != weapon)
        return (void)printf("BO3MacFix: zm inventory: no shield health yet: the shield isn't held\n");
    os_unfair_lock_lock(&shield_lock);
    shield_weapon = 0;
    os_unfair_lock_unlock(&shield_lock);
    // the engine's shield functions reach the server's BG state through this thread's bgs pointer
    uint64_t *bgs = BG_ThreadBgs();
    if (bgs == NULL || *bgs != base + ADDR_level_bgs)
        return (void)printf("BO3MacFix: zm inventory: no shield health: this thread's BG state (%#llx) isn't the "
                            "server's\n", bgs != NULL ? (unsigned long long)*bgs : 0ULL);
    int32_t client_num = 0;
    safe_read(client + gclient_ClientNum, &client_num, sizeof(client_num));
    int before = BG_GetShieldHealth(-1, client_num, weapon);
    G_SetShieldHealth(ent, health);
    int now = BG_GetShieldHealth(-1, client_num, weapon), stored = -1;
    stored_shield_health(client, &stored);
    // AAE's _zm_weap_riotshield.gsc: (health, full) -> zmInventory.shield_health = health / full, after its model update
    script_handle_t handle;
    bool hud = script_function_hashed(&handle, "scripts/zm/_zm_weap_riotshield", loadout_script_hash("riotshield"),
                                      Aae_ShieldHud);
    if (hud) {
        // the last parameter first
        Scr_AddInt(Script_Server, full);
        Scr_AddInt(Script_Server, now);
        Scr_FreeThread(Script_Server, Scr_ExecEntThread(ent, 2, handle));
    }
    char name[72] = "?";
    weapon_script_name(weapon, name, sizeof(name));
    printf("BO3MacFix: zm inventory: %s's health %s: %d of %d (was %d; stored %d)%s\n", name,
           now == health && stored == now ? "back" : "NOT back", now, full, before, stored,
           hud ? ", the HUD's meter set" : ", no HUD function (not AAE's riotshield script)");
}

// the shield's step of the poll: NULL nothing waiting, "" waiting, else the command to send
static const char *shield_poll(uint64_t client, bool readable) {
    os_unfair_lock_lock(&shield_lock);
    uint64_t weapon = shield_weapon;
    os_unfair_lock_unlock(&shield_lock);
    if (weapon == 0)
        return NULL;
    int full = 0;
    bool held = readable && held_shield(client, &full) == weapon;
    long long now = now_ms();
    const char *step = "";
    bool expired = false;
    os_unfair_lock_lock(&shield_lock);
    if (shield_weapon != weapon) // (a restore since the read above)
        step = shield_weapon != 0 ? "" : NULL;
    else if (now >= shield_until) {
        shield_weapon = 0;
        step = NULL;
        expired = true;
    } else if (!held)
        shield_held_polls = 0;
    // held for two polls, so the give's own HUD update (full) has run
    else if (++shield_held_polls >= 2 && now >= shield_sent_at + Shield_Resend_Ms) {
        shield_sent_at = now;
        shield_command_until = now + Switch_Ms;
        step = "give " Shield_Command;
    }
    os_unfair_lock_unlock(&shield_lock);
    if (expired)
        printf("BO3MacFix: zm inventory: the shield wasn't held (or its command didn't run) within %d s of the "
               "restore: its health stays full\n", Shield_Wait_Ms / 1000);
    return step;
}

const char *zm_inventory_switch_poll(void) {
    uint64_t first = entities(), client = 0, in_hand = 0;
    char why[64] = "no player", raw[96] = "";
    bool readable = first != 0 && safe_read(first + gentity_Client, &client, sizeof(client)) && client != 0 &&
                    safe_read(client + gclient_Weapon, &in_hand, sizeof(in_hand));
    bool controlled = readable && player_controls(first, client, why, sizeof(why), raw, sizeof(raw));
    const char *shield = shield_poll(client, readable);
    os_unfair_lock_lock(&restore_lock);
    uint64_t weapon = switch_watch.weapon;
    os_unfair_lock_unlock(&restore_lock);
    if (weapon == 0)
        return shield;
    bool held = readable && holds(client, weapon);
    char state[64], name[72] = "none";
    if (in_hand != 0)
        weapon_script_name(in_hand, name, sizeof(name));
    // what's logged when it changes: control, and the gun in hand
    snprintf(state, sizeof(state), "%s, %s", controlled ? "in control" : why, name);
    os_unfair_lock_lock(&restore_lock);
    // (a restore since the read above starts over)
    loadout_switch_action_t action = switch_watch.weapon != weapon ? Loadout_Switch_Wait :
        loadout_switch_poll(&switch_watch, controlled, in_hand, held);
    if (action == Loadout_Switch_Now) {
        switch_weapon = weapon;
        switch_until = now_ms() + Switch_Ms;
    }
    // the log tells when the player got control and what it had in hand then
    bool changed = strcmp(state, switch_state) != 0;
    snprintf(switch_state, sizeof(switch_state), "%s", state);
    os_unfair_lock_unlock(&restore_lock);
    if (changed)
        printf("BO3MacFix: zm inventory: after the restore: %s in hand%s (%s)\n", state,
               in_hand == weapon ? " (the saved one)" : "", raw);
    if (action == Loadout_Switch_Now)
        printf("BO3MacFix: zm inventory: %s in hand once the player had control, not the saved gun: switching back\n",
               name);
    if (action == Loadout_Switch_Now)
        return "give " Switch_Command;
    // the shield's command when the gun in hand has none this poll
    return shield != NULL && shield[0] != '\0' ? shield : action == Loadout_Switch_Done ? shield : "";
}

// CheatsOk(ent): whether ent may use cheat commands (sv_cheats is on and it's alive). BO3MacFix's own give and take
// commands for the local player get through as zm_inventory_allow_commands says; "give bo3macfix_restore"
// restores the loadout zm_inventory_set_restore set up, and then tells give it's done; everything else is the game's.
// The hook is reached by a jump (the pre-patched copy's stub or Dobby's), so the return address is the game's caller.
install_hook_name(CheatsOk, int, void *ent) {
    uint64_t from = (uint64_t)__builtin_return_address(0);
    int command = restore_command(from);
    if (command == 1)
        run_restore(ent);
    else if (command == 2)
        run_switch(ent);
    else if (command == 3)
        run_shield(ent);
    if (command != 0)
        return 0;
    long long held = 0;
    if (let_through(ent, from, &held)) {
        printf("BO3MacFix: zm inventory: let BO3MacFix's %s command past the cheat check (%.1f s after it was sent)\n",
               from == base + ADDR_CheatsOk_From_Give ? "give" : "take", held / 1000.0);
        return 1;
    }
    return orig_CheatsOk(ent);
}

void zm_inventory_init(uint64_t game_base) {
    base = game_base;
    G_GivePlayerWeapon = (void *)(base + ADDR_G_GivePlayerWeapon);
    G_GiveStartAmmo = (void *)(base + ADDR_G_GiveStartAmmo);
    G_TakePlayerWeapon = (void *)(base + ADDR_G_TakePlayerWeapon);
    G_SelectWeapon = (void *)(base + ADDR_G_SelectWeapon);
    G_SwitchToWeapon = (void *)(base + ADDR_G_SwitchToWeapon);
    BG_GetWeaponForName = (void *)(base + ADDR_BG_GetWeaponForName);
    BG_GetPerkIndexForName = (void *)(base + ADDR_BG_GetPerkIndexForName);
    BG_HasPerk = (void *)(base + ADDR_BG_HasPerk);
    SV_Cmd_ArgvBuffer = (void *)(base + ADDR_SV_Cmd_ArgvBuffer);
    Scr_GetFunctionHandle = (void *)(base + ADDR_Scr_GetFunctionHandle);
    Scr_ExecEntThread = (void *)(base + ADDR_Scr_ExecEntThread);
    Scr_FreeThread = (void *)(base + ADDR_Scr_FreeThread);
    Scr_AddString = (void *)(base + ADDR_Scr_AddString);
    Scr_AddWeapon = (void *)(base + ADDR_Scr_AddWeapon);
    Scr_AddInt = (void *)(base + ADDR_Scr_AddInt);
    BG_GetAmmoInClip = (void *)(base + ADDR_BG_GetAmmoInClip);
    BG_GetAmmoStock = (void *)(base + ADDR_BG_GetAmmoStock);
    BG_SetAmmoInClip = (void *)(base + ADDR_BG_SetAmmoInClip);
    BG_SetAmmoStock = (void *)(base + ADDR_BG_SetAmmoStock);
    BG_GetClipSize = (void *)(base + ADDR_BG_GetClipSize);
    BG_WeaponIsClipOnly = (void *)(base + ADDR_BG_WeaponIsClipOnly);
    BG_GetMaxAmmo = (void *)(base + ADDR_BG_GetMaxAmmo);
    Dvar_FindByHash = (void *)(base + ADDR_Dvar_FindByHash);
    Dvar_GetInt = (void *)(base + ADDR_Dvar_GetInt);
    Gadget_SlotWeapon = (void *)(base + ADDR_Gadget_SlotWeapon);
    G_GadgetPowerSet = (void *)(base + ADDR_G_GadgetPowerSet);
    G_SetShieldHealth = (void *)(base + ADDR_G_SetShieldHealth);
    BG_GetShieldHealth = (void *)(base + ADDR_BG_GetShieldHealth);
    BG_ThreadBgs = (void *)(base + ADDR_BG_ThreadBgs);
}

// at start-up, before the game's threads run: the pre-patched copy has a slot for it, so that only stores a pointer.
// Without one (a copy made by an older BO3MacFix, or the original executable) Dobby writes the game's code, and doing
// that while the game runs crashed it (live, as the first weapon was given)
void zm_inventory_install_hook(void) {
    if (base != 0)
        install_hook_CheatsOk((void *)(base + ADDR_CheatsOk));
}
