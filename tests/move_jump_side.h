// BO3MacFix's side of move_jump_cases.h: T7Overcharged's places as aae_move_jump.c has them, with jumpslowdown on
// (whoever includes this makes aae_movement_started() and aae_movement_patch_on(Move_JumpSlowdown) say so): the
// hooks, and the detours' handlers with the registers the game has at their places. For tests/test_aae_move_jump.c
// and for the comparison with T7Overcharged's own code (wip/agents/moveb/wine, not in the repository).

static detour_registers_t side_registers(uint64_t r12, uint64_t r13, uint64_t r14, uint64_t r15) {
    detour_registers_t registers;
    memset(&registers, 0, sizeof(registers));
    registers.r12 = r12, registers.r13 = r13, registers.r14 = r14, registers.r15 = r15;
    return registers;
}
static int side_view_dip(uint8_t *ps, float fall_height) {
    return fake_BG_GetViewDip(ps, fall_height);
}
static void side_slow_down(uint8_t *ps) {
    fake_Jump_SlowDown(ps);
}
static float side_reduce_friction(uint8_t *ps) {
    return fake_Jump_ReduceFriction(ps);
}
// the landing: the begin's detour (the pmove_t in r12, the pml_t in r13), the game's landing, the end's (the player
// state in r15)
static void side_crash_land(uint8_t *pm, uint8_t *pml) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    detour_registers_t begin = side_registers((uint64_t)pm, (uint64_t)pml, 0, 0);
    handler_Landing_Begin(&begin);
    cases_game_crash_land(pm, pml);
    detour_registers_t end = side_registers(0, 0, 0, (uint64_t)ps);
    handler_Landing_End(&end);
}
// Jump_Start: the game's own before its cap, the cap's detour (the cap in xmm0, the player state in r12, the pmove_t
// in r14), the game's cap with what the detour left in xmm0, and the rest
static void side_jump_start(uint8_t *pm, uint8_t *pml, float height) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    cases_game_jump_before_clamp(pm, pml, height);
    detour_registers_t registers = side_registers((uint64_t)ps, 0, (uint64_t)pm, 0);
    memcpy(registers.xmm[0], &cases_cap, sizeof(cases_cap));
    handler_Jump_SpeedCap(&registers);
    float cap;
    memcpy(&cap, registers.xmm[0], sizeof(cap));
    cases_game_clamp(pm, cap);
    cases_game_jump_after_clamp(pm);
}
