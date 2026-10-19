// A zombies player's loadout for a checkpoint (zm_loadout.c): each gun's full name (with its attachments), its weapon
// options (the camo and the rest, as GetWeaponOptions has them) and attachment cosmetic variants, its alternate ammo
// type, the gun in hand, and the perks. Plain C with the game's memory behind a read function, so
// tests/test_zm_loadout.c tests it with a made-up memory image and made-up game calls.
//
// Recording: the guns come from the player state (zm_inventory.c), the ammo types and perks from the server script
// VM's variables, where the zombies scripts keep them: self.aat[weapon] = name (aat_shared.gsc's acquire) and
// self.perks_active (_zm_perks.gsc's give_perk adds to it, perk_think takes lost ones out).
// Restoring: loadout_restore takes the starting guns that weren't saved, gives the saved ones with their options, then
// their ammo types and the perks through the game's own script functions, and switches to the gun that was in hand.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define Loadout_Max_Guns 8
#define Loadout_Max_Perks 24
#define Loadout_Name_Size 160 // a weapon name with its attachments
#define Loadout_Short_Size 48 // an ammo type's or a perk's name

typedef struct {
    char name[Loadout_Name_Size]; // like "ar_standard_upgraded_zm+reflex+extclip"
    uint64_t options;             // the weapon options: camo in the low byte (CalcWeaponOptions)
    uint32_t acvi;                // the attachments' cosmetic variants
    char aat[Loadout_Short_Size]; // like "zm_aat_blast_furnace", "" for none
} loadout_gun_t;

typedef struct {
    loadout_gun_t guns[Loadout_Max_Guns];
    int gun_count;
    int held; // the gun in hand, -1 for none
    char perks[Loadout_Max_Perks][Loadout_Short_Size]; // like "specialty_armorvest"
    int perk_count;
} loadout_t;

void loadout_clear(loadout_t *loadout);

// ---- names

// the game's canonical hash, which it names script functions, fields and namespaces by
uint32_t loadout_script_hash(const char *name);

// a weapon or perk name the game has (letters, digits, '_' and, between attachments, '+'), which can go into
// checkpoint.ini and a console command
bool loadout_valid_name(const char *name);

// the full name of a Weapon (index in the low 9 bits, then 8 attachments of 6 bits each): its variant's name and a
// "+<attachment>" for each attachment, from the game's attachment names (attachment_count of them, by id; id 0 is
// none). False if a name doesn't fit or isn't one.
bool loadout_weapon_name(const char *variant, uint64_t weapon, const char *const *attachment_names, int attachment_count,
                         char *out, size_t out_size);

// the same weapon name in the game's other form: without "_zm" after its variant if it has one, else with it, its
// attachments kept ("ray_gun_upgraded_zm+fmj" and "ray_gun_upgraded+fmj"). The server looks weapons up by the name a
// WeaponVariantDef has second, which for zombies' weapons is the one without "_zm". False if it doesn't fit.
bool loadout_other_name(const char *name, char *out, size_t out_size);

// ---- checkpoint.ini: "Guns = name:options:acvi:aat,..." (options in hex), "GunInHand = name", "PerkList = a,b"

// the guns as checkpoint.ini's Guns value; false if it doesn't fit
bool loadout_format_guns(const loadout_t *loadout, char *out, size_t out_size);
// the perks as a PerkList value
bool loadout_format_perks(const loadout_t *loadout, char *out, size_t out_size);
// a loadout from those values (in_hand: the gun in hand's name, NULL or empty for none). False, leaving loadout empty,
// if something is malformed.
bool loadout_parse(const char *guns, const char *in_hand, const char *perks, loadout_t *loadout);

// ---- the server script VM's variables, read-only

typedef bool (*loadout_read_t)(uint64_t address, void *out, size_t size);

typedef struct {
    loadout_read_t read; // false where nothing is mapped
    uint64_t var_glob;   // the script instance's variable globals (0x100 bytes: hash buckets, then the variables)
    uint64_t class_map;  // where the instance's class table pointer is
    uint64_t strings;    // where the string table pointer is
} loadout_vm_t;

// the script object of entity entnum (a player), 0 if it has none
uint32_t loadout_vm_entity(const loadout_vm_t *vm, int entnum);
// the variable that is object's field (or array element) with that key, 0 if there's none
uint32_t loadout_vm_find(const loadout_vm_t *vm, uint32_t object, uint64_t key);
// the array object entity's field holds, 0 if it isn't one
uint32_t loadout_vm_array_field(const loadout_vm_t *vm, uint32_t entity, const char *field);

typedef struct {
    uint64_t weapon;
    char aat[Loadout_Short_Size];
} loadout_aat_t;

// self.aat: each weapon's ammo type ("none" left out), at most max; -1 if it can't be read
int loadout_vm_read_aats(const loadout_vm_t *vm, uint32_t entity, loadout_aat_t *out, int max);
// the strings in an array of strings (self.perks_active), at most max; -1 if it can't be read
int loadout_vm_read_strings(const loadout_vm_t *vm, uint32_t array, char (*out)[Loadout_Short_Size], int max);

// ---- restoring

// the game, for loadout_restore; context is passed back to each
typedef struct {
    void *context;
    uint64_t (*weapon_for_name)(void *context, const char *name);  // 0 if there's no such weapon
    int (*primaries)(void *context, uint64_t *out, int max);       // the primary weapons held now
    bool (*take)(void *context, uint64_t weapon);
    bool (*give)(void *context, uint64_t weapon, uint64_t options, uint32_t acvi);
    bool (*acquire_aat)(void *context, uint64_t weapon, const char *aat); // aat::acquire(weapon, aat)
    bool (*has_perk)(void *context, const char *perk);
    bool (*give_perk)(void *context, const char *perk);                   // zm_perks::give_perk(perk, false)
    void (*switch_to)(void *context, uint64_t weapon);
} loadout_ops_t;

typedef struct {
    int taken, given, kept, aats, perks, perks_had, failed;
    char failures[256]; // what couldn't be restored, comma-separated
} loadout_result_t;

// turns the player's loadout into saved: takes the held primaries that aren't saved, gives each saved gun that isn't
// held (exactly: the same weapon and attachments) with its options, gives each saved gun its ammo type, gives the
// saved perks the player doesn't have, and switches to the gun that was in hand (the first one if none was)
void loadout_restore(const loadout_t *saved, const loadout_ops_t *ops, loadout_result_t *result);

// one line for the log: what a restore did
void loadout_result_text(const loadout_result_t *result, char *out, size_t out_size);
