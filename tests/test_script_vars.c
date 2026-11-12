// script_vars.c: chaining more entries onto the script variable table's free list, and the hook that gives the
// server a bigger table, against a made-up game whose setup and whose taking and returning of variables work as the
// game's do (the test includes the source itself).
#include <stdio.h>
#include <stdlib.h>
#include "../source/script_vars.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}

// the game's own three operations on a table
static void game_setup(uint8_t *table, uint32_t count) {
    memset(table, 0, (size_t)count * Script_Var_Size);
    for (uint32_t i = 0; i < count; i++) {
        put32(table + (size_t)i * Script_Var_Size + Script_Var_Next, i + 1 < count ? i + 1 : 0);
        put32(table + (size_t)i * Script_Var_Size + Script_Var_Type, Script_Var_Free);
    }
    put32(table + Script_Var_Type, 0);
}
static uint32_t game_take(uint8_t *table) {
    uint32_t id = get32(table + Script_Var_Next);
    if (id == 0)
        return 0; // "exceeded maximum number of child server script variables"
    put32(table + Script_Var_Next, get32(table + (size_t)id * Script_Var_Size + Script_Var_Next));
    put32(table + (size_t)id * Script_Var_Size + Script_Var_Type, 7);
    return id;
}
static void game_return(uint8_t *table, uint32_t id) {
    put32(table + (size_t)id * Script_Var_Size + Script_Var_Type, Script_Var_Free);
    put32(table + (size_t)id * Script_Var_Size + Script_Var_Next, get32(table + Script_Var_Next));
    put32(table + Script_Var_Next, id);
}

// the free chain's ids in order, checked to end and to hold only free entries below count
static uint32_t chain(const uint8_t *table, uint32_t count, uint32_t *ids) {
    uint32_t n = 0;
    for (uint32_t id = get32(table + Script_Var_Next); id != 0; id = get32(table + (size_t)id * Script_Var_Size + Script_Var_Next)) {
        if (id >= count || n >= count || get32(table + (size_t)id * Script_Var_Size + Script_Var_Type) != Script_Var_Free) {
            CHECK(!"a broken free chain");
            break;
        }
        ids[n++] = id;
    }
    return n;
}

static void test_extend(void) {
    enum { Old = 16, New = 40 };
    static uint8_t table[New * Script_Var_Size];
    uint32_t ids[New];

    // a table the game just set up: its own entries first, then the new ones
    game_setup(table, Old);
    script_vars_extend(table, Old, New, false);
    CHECK(chain(table, New, ids) == New - 1);
    for (uint32_t i = 0; i < New - 1; i++)
        CHECK(ids[i] == i + 1);
    CHECK(get32(table + Script_Var_Type) == 0);
    // every entry can be taken exactly once, in that order, then there are none
    for (uint32_t i = 1; i < New; i++)
        CHECK(game_take(table) == i);
    CHECK(game_take(table) == 0);
    game_return(table, 5), game_return(table, 30);
    CHECK(game_take(table) == 30 && game_take(table) == 5 && game_take(table) == 0);

    // the plugin's way: the new ones first
    game_setup(table, Old);
    script_vars_extend(table, Old, New, true);
    CHECK(chain(table, New, ids) == New - 1);
    for (uint32_t i = 0; i < New - Old; i++)
        CHECK(ids[i] == Old + i);
    for (uint32_t i = 0; i < Old - 1; i++)
        CHECK(ids[New - Old + i] == i + 1);

    // a table in use, where the last free entry isn't the table's last: the new ones still go behind it
    game_setup(table, Old);
    for (uint32_t i = 1; i < Old; i++)
        game_take(table);
    game_return(table, 3), game_return(table, 7);
    script_vars_extend(table, Old, New, false);
    CHECK(chain(table, New, ids) == 2 + New - Old && ids[0] == 7 && ids[1] == 3 && ids[2] == Old && ids[New - Old + 1] == New - 1);
    // and with no free entry left at all they are the whole chain
    game_setup(table, Old);
    for (uint32_t i = 1; i < Old; i++)
        game_take(table);
    script_vars_extend(table, Old, New, false);
    CHECK(chain(table, New, ids) == New - Old && ids[0] == Old);

    // nothing to add
    game_setup(table, Old);
    script_vars_extend(table, Old, Old, false);
    script_vars_extend(table, Old, 4, true);
    CHECK(chain(table, Old, ids) == Old - 1 && ids[0] == 1 && ids[Old - 2] == Old - 1);
}

// the game's memory around the two tables' pointers, and its setup function
static uint8_t var_glob[2 * VarGlob_Size];
static int setups;
static void game_init_variables(int inst) {
    setups++;
    game_setup(*table_slot(inst), inst == Script_Client ? Script_Vars_Client : Script_Vars_Server);
}

static void test_hook(void) {
    base_address = (uint64_t)var_glob - ADDR_scrVarGlob;
    orig_Scr_InitVariables = game_init_variables;
    uint8_t *server = malloc((size_t)Script_Vars_Server * Script_Var_Size);
    uint8_t *client = malloc((size_t)Script_Vars_Client * Script_Var_Size);
    *table_slot(Script_Server) = server;
    *table_slot(Script_Client) = client;
    uint32_t limit;

    // the client's table stays the game's
    fake_Scr_InitVariables(Script_Client);
    CHECK(*table_slot(Script_Client) == client && setups == 1);
    CHECK(script_variables_used(Script_Client, &limit) == 0 && limit == Script_Vars_Client);

    // the server's is swapped for a bigger one that starts as the game set its own up
    fake_Scr_InitVariables(Script_Server);
    uint8_t *raised = *table_slot(Script_Server);
    CHECK(raised != server && raised == raised_table && setups == 2);
    CHECK(script_variables_used(Script_Server, &limit) == 0 && limit == Script_Vars_Raised);
    for (uint32_t i = 1; i < Script_Vars_Raised; i++)
        if (game_take(raised) != i) {
            CHECK(!"the entries don't come out in order");
            break;
        }
    CHECK(game_take(raised) == 0);
    CHECK(script_variables_used(Script_Server, &limit) == Script_Vars_Raised - 1);
    game_return(raised, 200000);
    CHECK(script_variables_used(Script_Server, &limit) == Script_Vars_Raised - 2);

    // the next map: the game sets up its 130,000 in the bigger table, and the rest is chained on again
    fake_Scr_InitVariables(Script_Server);
    CHECK(*table_slot(Script_Server) == raised && setups == 3);
    CHECK(script_variables_used(Script_Server, &limit) == 0);
    CHECK(game_take(raised) == 1);
    for (uint32_t i = 2; i < Script_Vars_Raised; i++)
        game_take(raised);
    CHECK(game_take(raised) == 0);

    // should the game ever point at a table of its own again, that one is taken over the same way
    *table_slot(Script_Server) = server;
    fake_Scr_InitVariables(Script_Server);
    CHECK(*table_slot(Script_Server) == raised && game_take(raised) == 1);
    free(server);
    free(client);
}

int main(void) {
    test_extend();
    test_hook();
    if (failures != 0) {
        printf("test_script_vars: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_script_vars: all passed\n");
    return 0;
}
