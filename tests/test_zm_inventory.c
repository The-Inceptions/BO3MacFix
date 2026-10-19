// Tests zm_inventory.c's pass through the game's cheat check against a made-up g_entities: only the give and take
// commands get it, for the local player while alive, as many as were allowed and only for a while. Every other cheat
// command, and any other caller, gets the game's own answer.
#include "../source/zm_inventory.c"

// what zm_inventory.c links against; the hook is called directly here
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    return -1;
}

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAILED line %d: %s\n", __LINE__, #cond);                                                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Entity_Size 0x4f8
static uint64_t entities_address; // what the game's pointer at ADDR_g_entities_Ptr holds
_Alignas(8) static uint8_t g_entities[3 * Entity_Size];
static int game_checks;

// the game's own CheatsOk, with sv_cheats off
static int game_cheats_ok(void *ent) {
    game_checks++;
    return 0;
}

static void set_health(int entity, int health) {
    memcpy(g_entities + entity * Entity_Size + gentity_Health, &health, sizeof(health));
}

int main(void) {
    // a game image where ADDR_g_entities_Ptr is entities_address
    base = (uint64_t)&entities_address - ADDR_g_entities_Ptr;
    entities_address = (uint64_t)g_entities;
    orig_CheatsOk = game_cheats_ok;
    void *player = g_entities, *other = g_entities + Entity_Size;
    uint64_t give = base + ADDR_CheatsOk_From_Give, take = base + ADDR_CheatsOk_From_Take;
    uint64_t god = base + 0xf4371d; // another cheat command's call: god, at 0xf43718
    set_health(0, 100);
    set_health(1, 100);

    // nothing allowed: the game's answer
    CHECK(!let_through(player, give));

    // two allowed: give and take get through, nothing else does or uses them up
    zm_inventory_allow_cheats(2);
    CHECK(!let_through(player, god));
    CHECK(!let_through(other, give)); // another player in a game hosted here
    CHECK(!let_through(NULL, give));
    set_health(0, 0);
    CHECK(!let_through(player, give)); // dead, as the game's check wants too
    set_health(0, 100);
    CHECK(atomic_load(&allowed) == 2);
    CHECK(let_through(player, take));
    CHECK(let_through(player, give));
    CHECK(!let_through(player, give)); // both used
    CHECK(atomic_load(&allowed) == 0);

    // the hook itself, called from here rather than from give or take: the game's own check, allowance or not
    zm_inventory_allow_cheats(1);
    game_checks = 0;
    CHECK(fake_CheatsOk(player) == 0 && game_checks == 1);
    CHECK(atomic_load(&allowed) == 1);

    // only for a while
    atomic_store(&allowed_until, now_ms() - 1);
    CHECK(!let_through(player, give));

    // no g_entities to compare with: nothing gets through
    zm_inventory_allow_cheats(1);
    entities_address = 0;
    CHECK(!let_through(player, give));
    entities_address = (uint64_t)g_entities;
    CHECK(let_through(player, give));

    if (failures == 0)
        printf("test_zm_inventory: all passed\n");
    return failures == 0 ? 0 : 1;
}
