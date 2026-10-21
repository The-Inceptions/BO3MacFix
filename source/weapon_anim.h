// T7Overcharged's startweaponanim(localclientnum, state, hand), for All-around Enhancement's first raise on inspect,
// after its ammo refill, and its low ready (t7overcharged.c). The state is a weapon animation state, the value the
// weapon state machine puts in playerState.handState[hand].weapAnim: AAE's firstraise 38, reloadempty 40 and lowready
// 68 map (BG_MapWeaponAnimStateToAnimIndex, 0x10137eeec) to the weapon's anims 51 firstRaise, 41 reloadEmpty and
// 79 lowReadyLoop. The client's viewmodel update (0x100d87230) starts a hand's animation when the predicted player
// state's weapAnim differs from the one it last started, then remembers it. We make it start ours that way: for one
// call, the predicted state holds our state with the toggle bit flipped, and afterwards the real value goes back in
// both places, so the next frame doesn't restart the real one and ours plays until the server's weapAnim changes.
#ifndef WEAPON_ANIM_H
#define WEAPON_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#define WeaponAnim_StateMask 0x1fff
#define WeaponAnim_Toggle 0x2000
// the highest state BG_MapWeaponAnimStateToAnimIndex maps (its jump table at 0x10137f898)
#define WeaponAnim_MaxState 0xa1

static inline bool weapon_anim_request_valid(int hand, int state) {
    return (hand == 0 || hand == 1) && state >= 1 && state <= WeaponAnim_MaxState;
}

// only while the engine has nothing new of its own to start for the hand: otherwise the server's animation wins
static inline bool weapon_anim_can_inject(uint16_t real, uint16_t last_started) {
    return real == last_started;
}

// what the predicted weapAnim holds for that call: the state, the real value's other bits, and the toggle bit flipped
// from the one last started, so it differs from it even when the state is the same
static inline uint16_t weapon_anim_injected(uint16_t real, uint16_t last_started, int state) {
    return (uint16_t)((real & ~(WeaponAnim_StateMask | WeaponAnim_Toggle)) |
                      ((last_started ^ WeaponAnim_Toggle) & WeaponAnim_Toggle) | (state & WeaponAnim_StateMask));
}

#endif
