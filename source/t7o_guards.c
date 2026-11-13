// T7OCrashGuards: two of T7Overcharged's fixes for crashes of the game itself.
//
// T7Overcharged, which All-around Enhancement loads on Windows, guards a few places where the game crashes by itself.
// The build the mod ships names more such places than it guards: it looks them all up for its report of what it
// found, but only applies four of them. Two of those are plain guards that are right on the Mac as well, and they are
// here; wip/night/impl-crash-guards.md has all of them, with why the others aren't.
//
// - Its bg_weapon_attachment_fix ("bg/weapon_attachment_null_guard"). When the client registers a weapon with
//   attachments (CG_RegisterWeapon), it works out the weapon's 197 fields one by one: an attachment that overrides a
//   field gives its own value, which it marks in a bit of its entry in the weapon's table of attachments. For eight
//   of the fields (0x68 to 0x6c, 0xad to 0xaf) the attachment that changes the weapon's mode (the first of its
//   attachments whose kind is 1, an underbarrel for instance) is asked first, and that one read of the table doesn't
//   look whether the entry is there. Its number comes from another table of the weapon, so a weapon (a mod's,
//   typically) whose mode attachment has no entry crashes the game at that read, on the Mac as on Windows without
//   T7Overcharged. The function's seven other reads of the table look first, and so do the game's others around it.
//   T7Overcharged makes the missing entry one that overrides nothing: the game goes on with the weapon's other
//   attachments, as its own code does for theirs. So does this: at the read, a missing entry is replaced by a
//   stand-in whose override bits are all clear.
// - Its local_client_bounds_fix. Com_LocalClient_GetControllerIndex(localClientNum) looks the number up in the
//   game's table of its two local clients without a bounds test. Lua functions ask it for -1 when a controller
//   belongs to no local client, which reads the bytes before the table, and a bigger number is read as far past it.
//   T7Overcharged answers -1 (no controller, the function's own answer for a client the table doesn't have) for
//   anything but 0 and 1, and so does this.
//
// Both apply once the mod has loaded T7Overcharged, as on Windows (the stand-in's init has been asked for, see
// t7o_mod_folder); before that, and without the mod, the game's own code runs as it is. Each guard counts what it
// caught, and log.txt says so in lines starting with "T7O crash guards:", the first 16 times and then at each power of
// two. The weapon guard is a detour inside a function, so only the pre-patched executable has it; the bounds guard is
// a hook on a function. BO3MACFIX_T7OGUARDSDEBUG=1 also logs every catch and, at each power of two, how often the
// guarded places were passed.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "detour.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"
#include "t7o_guards.h"
#include "t7overcharged.h"

#define Attachment_Overrides 0x3c0 // in an attachment's entry: a bit for each of the weapon's fields it overrides
#define Override_Words 7           // 32-bit words of those bits: 197 fields (CG_RegisterWeapon's loop over them)
#define Local_Clients 2            // the game's table of local clients has 2 entries (its reset clears both)

static int debug;

// the stand-in for a missing attachment entry: the override bits read through it are all clear
static const uint8_t no_attachment[Attachment_Overrides + Override_Words * 4];

bool t7o_guards_logged(uint32_t n) {
    return n <= 16 || (n & (n - 1)) == 0;
}

static bool t7o_loaded(void) {
    return t7o_mod_folder()[0] != '\0';
}

#pragma mark - the weapon guard

const void *t7o_guards_attachment_entry(const void *entry, uint64_t word, bool t7o_loaded) {
    if (entry != NULL)
        return entry;
    // the stand-in only has the bits of the 197 fields; any other word would be read from what lies behind it
    if (!t7o_loaded || word >= Override_Words)
        return NULL;
    return no_attachment;
}

static atomic_uint attachments_missing, attachment_reads;

// "mov ecx, [rax + 4*rcx + 0x3c0]" in the function that works out a weapon's field (see offsets.h): rax is the
// attachment's entry from the weapon's table, rcx which word of its override bits, r12 the field, rbx the attachment's
// number and r15 the weapon (its number in the low 9 bits)
detour_with_registers(BG_WeaponAttachment_Overrides) {
    if (debug) {
        uint32_t reads = atomic_fetch_add_explicit(&attachment_reads, 1, memory_order_relaxed) + 1;
        if ((reads & (reads - 1)) == 0)
            printf("T7O crash guards: the weapon attachment read has been passed %u times\n", reads);
    }
    const void *entry = t7o_guards_attachment_entry((const void *)registers->rax, registers->rcx, t7o_loaded());
    if (entry == (const void *)registers->rax)
        return;
    registers->rax = (uint64_t)entry;
    uint32_t n = atomic_fetch_add_explicit(&attachments_missing, 1, memory_order_relaxed) + 1;
    if (debug || t7o_guards_logged(n))
        printf("T7O crash guards: weapon %u's attachment %u has no entry in the weapon's table, and the game read it "
               "for field %u, which would have crashed it; taken as overriding nothing, as T7Overcharged does "
               "(%u so far)\n",
               (unsigned)(registers->r15 & 0x1ff), (unsigned)registers->rbx, (unsigned)registers->r12, n);
}

#pragma mark - the bounds guard

bool t7o_guards_local_client_in_table(int32_t local_client) {
    return (uint32_t)local_client < Local_Clients;
}

static atomic_uint no_local_client, local_clients_out_of_table, controller_lookups;

install_hook_name(Com_LocalClient_GetControllerIndex, int32_t, int32_t local_client) {
    if (debug) {
        uint32_t lookups = atomic_fetch_add_explicit(&controller_lookups, 1, memory_order_relaxed) + 1;
        if ((lookups & (lookups - 1)) == 0)
            printf("T7O crash guards: local clients' controllers looked up %u times\n", lookups);
    }
    if (t7o_guards_local_client_in_table(local_client) || !t7o_loaded())
        return orig_Com_LocalClient_GetControllerIndex(local_client);
    // T7Overcharged tells -1 (what the game's own lookup gives for a controller no local client has) from the rest
    // only by a flag it sets for the rest and never reads; here they're counted apart, -1 is common
    atomic_uint *counter = local_client == -1 ? &no_local_client : &local_clients_out_of_table;
    uint32_t n = atomic_fetch_add_explicit(counter, 1, memory_order_relaxed) + 1;
    if (debug || t7o_guards_logged(n))
        printf("T7O crash guards: a local client's controller was asked for local client %d, outside the game's "
               "table of 2 (the game would have read outside it); answered -1, no controller, as T7Overcharged "
               "does (%u so far for %s)\n",
               local_client, n, local_client == -1 ? "-1" : "numbers other than -1");
    return -1;
}

void t7o_guards_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    const char *env = getenv("BO3MACFIX_T7OGUARDSDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    install_hook_Com_LocalClient_GetControllerIndex(
        (void *)(game_base_address + ADDR_Com_LocalClient_GetControllerIndex));
    if (prepatch_has_hook(ADDR_BG_WeaponAttachment_Overrides)) {
        install_detour_BG_WeaponAttachment_Overrides((void *)(game_base_address + ADDR_BG_WeaponAttachment_Overrides));
    } else {
        printf("T7O crash guards: not running from the pre-patched executable: a weapon whose mode attachment has no "
               "entry still crashes the game as it registers the weapon\n");
    }
    if (debug)
        printf("T7O crash guards: on, from when the mod loads T7Overcharged (weapon attachment guard %s)\n",
               prepatch_has_hook(ADDR_BG_WeaponAttachment_Overrides) ? "on" : "off");
}
