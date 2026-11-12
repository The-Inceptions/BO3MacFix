// The script VM's variables, and a higher limit for the server's (MoreScriptVariables). See script_vars.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define Script_Vars_Server 130000 // what the game gives the server's scripts
#define Script_Vars_Client 65000  // and the client's
#define Script_Vars_Raised 0x80000 // what All-around Enhancement's Windows plugin gives the server's
#define Script_Var_Size 0x40
#define Script_Var_Type 0x08 // Script_Var_Free for one that isn't in use
#define Script_Var_Next 0x18 // a free variable's: the next free one, 0 at the end; entry 0's is the first free one
#define Script_Var_Free 0x1b

// Chains the entries from old_count up to new_count onto the free list of a table the game has just set up (plain
// memory, tested by tests/test_script_vars.c): at its end, so the game's own entries are used first, or at_head as
// the plugin does, so the new ones are. The table must have room for new_count entries.
void script_vars_extend(uint8_t *table, uint32_t old_count, uint32_t new_count, bool at_head);

// hooks the game's setup of the tables; raise gives the server's Script_Vars_Raised entries
void script_vars_init(uint64_t game_base_address, int raise);
// how many of a script instance's variables are in use and how many it has (0 the server's, 1 the client's), for
// FrameStats' detail
uint32_t script_variables_used(int inst, uint32_t *limit);
