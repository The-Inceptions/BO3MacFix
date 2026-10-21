// weapon_anim.h: which startweaponanim requests are taken, and the weapAnim value that makes the engine start one
#include "../source/weapon_anim.h"
#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    // AAE's firstraise, reloadempty and lowready, on either hand
    CHECK(weapon_anim_request_valid(0, 38));
    CHECK(weapon_anim_request_valid(0, 40));
    CHECK(weapon_anim_request_valid(1, 68));
    CHECK(weapon_anim_request_valid(0, WeaponAnim_MaxState));
    CHECK(!weapon_anim_request_valid(0, 0));
    CHECK(!weapon_anim_request_valid(0, WeaponAnim_MaxState + 1));
    CHECK(!weapon_anim_request_valid(0, -1));
    CHECK(!weapon_anim_request_valid(2, 38));
    CHECK(!weapon_anim_request_valid(-1, 38));

    // only when the engine has already started the real one
    CHECK(weapon_anim_can_inject(0x2001, 0x2001));
    CHECK(!weapon_anim_can_inject(0x0003, 0x2001));

    // idle (1) with the toggle bit set or clear, and other high bits kept
    const uint16_t reals[] = { 0x0001, 0x2001, 0x4001, 0xe001, 0x0026 };
    const int states[] = { 1, 38, 40, 68, WeaponAnim_MaxState };
    for (unsigned int r = 0; r < sizeof(reals) / sizeof(reals[0]); r++) {
        for (unsigned int s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
            uint16_t real = reals[r];
            uint16_t injected = weapon_anim_injected(real, real, states[s]);
            CHECK(injected != real); // the engine sees a change, even for the state it's in
            CHECK((injected & WeaponAnim_StateMask) == states[s]);
            CHECK(((injected ^ real) & WeaponAnim_Toggle) != 0);
            CHECK((injected & ~(WeaponAnim_StateMask | WeaponAnim_Toggle)) ==
                  (real & ~(WeaponAnim_StateMask | WeaponAnim_Toggle)));
        }
    }
    // the engine's own forced restart value ((last | 0x1fff) ^ 0x2000, 0x100d8777a) still differs
    uint16_t forced = (uint16_t)((0x2001 | WeaponAnim_StateMask) ^ WeaponAnim_Toggle);
    CHECK(weapon_anim_injected(0x2001, forced, 38) != forced);

    if (failures == 0)
        printf("test_weapon_anim: all passed\n");
    return failures != 0;
}
