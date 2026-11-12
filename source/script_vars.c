// The script VM's variables, and a higher limit for the server's.
//
// Every value a script holds (a field, an array element, a thread, an object) takes one of the game's script
// variables: 0x40 bytes each, in one table per script instance, 130,000 for the server and 65,000 for the client. The
// free ones are a chain through the table (a variable's +0x18 names the next, entry 0's the first), taken from and
// put back at its head. When the server's run out, the game stops the match with "exceeded maximum number of child
// server script variables".
//
// All-around Enhancement 4 fills them much further than the game alone does: solo, Shadows of Evil starts round 7
// with 80,000 in use and Origins with 69,000. Its Windows plugin gives the server 524,288 (its gsc_memory component):
// after the game has set its table up, the plugin copies it into a bigger one, points the game at that and chains the
// new entries onto the free list. MoreScriptVariables does the same, with one difference: the plugin puts the new
// entries at the head of the chain, this puts them at its end. So below the game's own limit nothing changes at all,
// and at the limit the match goes on instead of stopping.
//
// What the game does over its whole table (cleaning up at the end of a map, its debugging dumps, 6 loops that know
// the 130,000) still only covers its own entries. That's the same with the plugin, where nearly every variable lies
// beyond them from the first moment. BO3MACFIX_SCRIPTVARSATHEAD=1 chains them the plugin's way, to try exactly that.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "dobby.h"
#include "offsets.h"
#include "script_vars.h"

#define Script_Server 0
#define Script_Client 1
#define VarGlob_Size 0x100
#define VarGlob_Vars 0x80

static uint64_t base_address;
static uint8_t *raised_table; // the server's bigger table, once made
static bool at_head;

static uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

void script_vars_extend(uint8_t *table, uint32_t old_count, uint32_t new_count, bool head) {
    if (new_count <= old_count || old_count < 2)
        return;
    // the new entries: free, each naming the next
    memset(table + (size_t)old_count * Script_Var_Size, 0, (size_t)(new_count - old_count) * Script_Var_Size);
    for (uint32_t i = old_count; i < new_count; i++) {
        uint8_t *entry = table + (size_t)i * Script_Var_Size;
        put32(entry + Script_Var_Type, Script_Var_Free);
        put32(entry + Script_Var_Next, i + 1 < new_count ? i + 1 : 0);
    }
    if (head) {
        // new first, then what was free before
        put32(table + (size_t)(new_count - 1) * Script_Var_Size + Script_Var_Next, get32(table + Script_Var_Next));
        put32(table + Script_Var_Next, old_count);
        return;
    }
    // behind the last free entry: the game's setup leaves that the table's last, otherwise walk the chain
    uint32_t last = old_count - 1;
    const uint8_t *entry = table + (size_t)last * Script_Var_Size;
    if (get32(entry + Script_Var_Type) != Script_Var_Free || get32(entry + Script_Var_Next) != 0) {
        last = 0;
        for (uint32_t steps = 0, next; steps < old_count; steps++, last = next) {
            next = get32(table + (size_t)last * Script_Var_Size + Script_Var_Next);
            if (next == 0 || next >= old_count)
                break;
        }
    }
    put32(table + (size_t)last * Script_Var_Size + Script_Var_Next, old_count);
}

static uint8_t **table_slot(int inst) {
    return (uint8_t **)(base_address + ADDR_scrVarGlob + (uint64_t)inst * VarGlob_Size + VarGlob_Vars);
}

// Scr_InitVariables(inst): clears the instance's table and chains all its entries as free; at launch (right after
// the table is allocated) and whenever the script VM starts over for a map
install_hook_name(Scr_InitVariables, void, int inst) {
    orig_Scr_InitVariables(inst);
    uint8_t **slot = table_slot(inst);
    if (inst != Script_Server || *slot == NULL)
        return;
    if (*slot != raised_table) {
        if (raised_table == NULL) {
            void *block = mmap(NULL, (size_t)Script_Vars_Raised * Script_Var_Size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
            if (block == MAP_FAILED) {
                printf("script variables: no memory for a bigger table, the server keeps %d\n", Script_Vars_Server);
                return;
            }
            raised_table = block;
            printf("script variables: the server has %d instead of %d\n", Script_Vars_Raised, Script_Vars_Server);
        }
        // what the game just set up, then the game looks there from now on (it reads the pointer every time)
        memcpy(raised_table, *slot, (size_t)Script_Vars_Server * Script_Var_Size);
        *slot = raised_table;
    }
    script_vars_extend(raised_table, Script_Vars_Server, Script_Vars_Raised, at_head);
}

uint32_t script_variables_used(int inst, uint32_t *limit) {
    *limit = inst == Script_Client ? Script_Vars_Client : Script_Vars_Server;
    if (base_address == 0 || *table_slot(inst) == NULL)
        return 0;
    const uint8_t *table = *table_slot(inst);
    if (table == raised_table)
        *limit = Script_Vars_Raised;
    uint32_t used = 0;
    for (uint32_t i = 1; i < *limit; i++)
        used += get32(table + (size_t)i * Script_Var_Size + Script_Var_Type) != Script_Var_Free;
    return used;
}

void script_vars_init(uint64_t game_base_address, int raise) {
    base_address = game_base_address;
    if (!raise)
        return;
    const char *head = getenv("BO3MACFIX_SCRIPTVARSATHEAD");
    at_head = head != NULL && atoi(head) != 0;
    if (at_head)
        printf("script variables: the new ones are used first (BO3MACFIX_SCRIPTVARSATHEAD)\n");
    install_hook_Scr_InitVariables((void *)(base_address + ADDR_Scr_InitVariables));
}
