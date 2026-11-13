// BO3MacFix's side of linkcam_cases.h: aae_linkcam.c's own functions on a made-up game laid out as the Mac's. For
// code that includes aae_plugin.c (with the mod and its plugin there) and aae_linkcam.c, and then linkcam_cases.h;
// side_cases_run runs the cases and puts back what it changed.
#include <stdlib.h>

static uint8_t *side_cgs; // two local clients' cg_t
static uint8_t side_cents[Cases_Cents][0x908];
static int32_t side_local_clients = Cases_Locals;

static uint8_t *side_cg(int local) {
    return side_cgs + (size_t)local * Cg_Size;
}
static cases_list_t *side_list(int local) {
    return (cases_list_t *)(side_cg(local) + Cg_Camera_List);
}
static int32_t *side_time(int local) {
    return (int32_t *)(side_cg(local) + Cg_Time);
}
static float *side_view_origin(int local) {
    return (float *)(side_cg(local) + Cg_View_Origin);
}
static float *side_view_axis(int local) {
    return (float *)(side_cg(local) + Cg_View_Axis);
}
static float *side_view_angles(int local) {
    return (float *)(side_cg(local) + Cg_View_Angles);
}
static uint8_t *side_cent(int index) {
    return side_cents[index];
}
static void side_reset(void) {
    memset(&state, 0, sizeof(state));
}

// the game's functions
static void side_game_link(uint64_t instance, uint64_t entity, uint64_t third, uint64_t fourth, uint64_t fifth,
                           uint64_t sixth) {
    (void)instance, (void)fourth, (void)fifth, (void)sixth;
    cases_stock_link((int)(third >> 32), (int32_t)entity);
}
static void side_game_delete(uint64_t instance, uint64_t entity, uint64_t third, uint64_t fourth, uint64_t fifth,
                             uint64_t sixth) {
    (void)instance, (void)third, (void)fourth, (void)fifth, (void)sixth;
    cases_stock_delete(0, (int32_t)entity);
}
static void side_game_force_delete(uint64_t instance, uint64_t entity, uint64_t third, uint64_t fourth,
                                   uint64_t fifth, uint64_t sixth) {
    (void)instance, (void)third, (void)fourth, (void)fifth, (void)sixth;
    cases_stock_delete(1, (int32_t)entity);
}

static void side_link(int local, int32_t entnum) {
    fake_CScr_LinkToCamera(1, (uint32_t)entnum, (uint64_t)(uint32_t)local << 32, 0, 0, 0);
}
static void side_delete(int force, int local, int32_t entnum) {
    if (force)
        fake_CScr_ForceDelete(1, (uint32_t)entnum, (uint64_t)(uint32_t)local << 32, 0, 0, 0);
    else
        fake_CScr_Delete(1, (uint32_t)entnum, (uint64_t)(uint32_t)local << 32, 0, 0, 0);
}
static void side_draw(void *object, uint8_t *cent, int32_t entnum, uint32_t flags, const uint8_t *lighting) {
    fake_AddDObjToScene(object, cent, entnum, flags, lighting);
}
// the game's own AnglesToAxis for the view, then what comes right behind it
static void side_view(int local) {
    AnglesToAxis(side_view_angles(local), side_view_axis(local));
    detour_registers_t registers;
    memset(&registers, 0, sizeof(registers));
    registers.r12 = (uint64_t)side_cg(local);
    handler_View_Axis(&registers);
}
static void side_noted(int32_t *count, int32_t entnum[4]) {
    *count = state.noted.count;
    memcpy(entnum, state.noted.entnum, sizeof(state.noted.entnum));
}
static uint32_t side_held(void) {
    return state.held.count;
}

static void side_cases_run(void) {
    uint8_t *const *kept_cg_clients = cg_clients;
    const int32_t *kept_local_client_count = local_client_count;
    void (*kept_axis)(const float *, float *) = AnglesToAxis;
    void (*kept_pose)(int, int32_t) = CG_EntityPoseChanged;
    typeof(orig_AddDObjToScene) kept_draw = orig_AddDObjToScene;
    typeof(orig_CScr_LinkToCamera) kept_link = orig_CScr_LinkToCamera;
    typeof(orig_CScr_Delete) kept_delete = orig_CScr_Delete;
    typeof(orig_CScr_ForceDelete) kept_force_delete = orig_CScr_ForceDelete;
    side_cgs = calloc(Cases_Locals, Cg_Size);
    cg_clients = &side_cgs;
    local_client_count = &side_local_clients;
    AnglesToAxis = cases_angles_to_axis;
    CG_EntityPoseChanged = cases_pose_changed;
    orig_AddDObjToScene = cases_drawn;
    orig_CScr_LinkToCamera = side_game_link;
    orig_CScr_Delete = side_game_delete;
    orig_CScr_ForceDelete = side_game_force_delete;
    cases_run();
    free(side_cgs);
    side_cgs = NULL;
    cg_clients = kept_cg_clients;
    local_client_count = kept_local_client_count;
    AnglesToAxis = kept_axis;
    CG_EntityPoseChanged = kept_pose;
    orig_AddDObjToScene = kept_draw;
    orig_CScr_LinkToCamera = kept_link;
    orig_CScr_Delete = kept_delete;
    orig_CScr_ForceDelete = kept_force_delete;
    memset(&state, 0, sizeof(state));
}
