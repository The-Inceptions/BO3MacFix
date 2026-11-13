// aae_linkcam.c: the plugin's rules for entities linked to the camera, its sums, and the hooks against a made-up game.
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_plugin.c"
#include "../source/aae_linkcam.c"

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static bool have_stubs = true;
bool prepatch_has_hook(uint32_t offset) {
    (void)offset;
    return have_stubs;
}
static struct {
    void *address, *replacement, **original;
} hooked[8];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    return 0;
}
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

// ---- the mod with the plugin
static char loaded_name[Mod_Name_Size + 8], plugin_file[4096 + 64];
static uint64_t plugin_time = 5000;
static uint64_t test_plugin_now(void) {
    return plugin_time;
}
static void set_mod(bool loaded) {
    memset(loaded_name, 0, sizeof(loaded_name));
    if (loaded)
        snprintf(loaded_name, sizeof(loaded_name), "%s", strrchr(mod_folder, '/') + 1);
    plugin_time += Plugin_Look_Ms;
}

// ---- the made-up game: two local clients' cg_t (and what comes after them), client entities, and the game's
// functions, which note what they were asked
static uint8_t *game_cgs;
static int32_t game_local_clients = 1;
static uint8_t game_cents[8][0x908];
static aae_linkcam_list_t *list(int local) {
    return list_of(game_cgs + (size_t)local * Cg_Size);
}
static uint8_t *cg_of(int local) {
    return game_cgs + (size_t)local * Cg_Size;
}

#define Log_Size 64
static struct {
    int count;
    struct {
        enum { Drawn, Axis, Pose, Linked, Deleted, Force_Deleted } kind;
        void *object;
        uint8_t *cent;
        int32_t entnum;
        uint32_t flags;
        const uint8_t *lighting;
        uint8_t lighting_copy[32], extra_copy[48];
        bool had_extra;
        float origin[3], angles[3]; // the entity's when it was drawn, the angles an axis was made from
        int local;
        uint64_t arguments[6];
    } call[Log_Size];
} game_log;
static void log_clear(void) {
    memset(&game_log, 0, sizeof(game_log));
}
static typeof(game_log.call[0]) *log_add(int kind) {
    static typeof(game_log.call[0]) spare;
    if (game_log.count >= Log_Size)
        return &spare;
    typeof(game_log.call[0]) *call = &game_log.call[game_log.count++];
    call->kind = kind;
    return call;
}
static int log_count(int kind) {
    int n = 0;
    for (int i = 0; i < game_log.count; i++)
        n += game_log.call[i].kind == (typeof(game_log.call[0].kind))kind;
    return n;
}
static typeof(game_log.call[0]) *log_find(int kind, int nth) {
    for (int i = 0; i < game_log.count; i++)
        if (game_log.call[i].kind == (typeof(game_log.call[0].kind))kind && nth-- == 0)
            return &game_log.call[i];
    return NULL;
}

static void game_draw(void *object, uint8_t *cent, int32_t entnum, uint32_t flags, const uint8_t *lighting) {
    typeof(game_log.call[0]) *call = log_add(Drawn);
    call->object = object, call->cent = cent, call->entnum = entnum, call->flags = flags, call->lighting = lighting;
    if (cent != NULL) {
        memcpy(call->origin, cent + Cent_Origin, sizeof(call->origin));
        memcpy(call->angles, cent + Cent_Angles, sizeof(call->angles));
    }
    if (lighting != NULL) {
        const uint8_t *extra;
        memcpy(call->lighting_copy, lighting, sizeof(call->lighting_copy));
        memcpy(&extra, lighting + Lighting_Extra, sizeof(extra));
        if ((lighting[0] & Lighting_Has_Extra) != 0 && extra != NULL) {
            memcpy(call->extra_copy, extra, sizeof(call->extra_copy));
            call->had_extra = true;
        }
    }
}
// an axis from angles: rows that tell the angles apart, so a test sees which ones it was made from
static void game_axis(const float *angles, float *axis) {
    typeof(game_log.call[0]) *call = log_add(Axis);
    memcpy(call->angles, angles, sizeof(call->angles));
    for (int k = 0; k < 9; k++)
        axis[k] = angles[k % 3] + (float)(k + 1) * 1000.0f;
}
static void game_pose(int local, int32_t entnum) {
    typeof(game_log.call[0]) *call = log_add(Pose);
    call->local = local, call->entnum = entnum;
}
// linktocamera's own part: what the method was given, and the game's script error at a fifth entity
static uint32_t link_mode = 4;
static float link_offset[3];
static jmp_buf script_error;
static void game_link(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    typeof(game_log.call[0]) *call = log_add(Linked);
    call->arguments[0] = a, call->arguments[1] = b, call->arguments[2] = c, call->arguments[3] = d;
    call->arguments[4] = e, call->arguments[5] = f;
    uint8_t *cg = cg_of_method(c);
    aae_linkcam_list_t *to = list_of(cg);
    if (to->count > 3)
        longjmp(script_error, 1);
    to->entnum[to->count] = (int32_t)b;
    to->mode[to->count] = link_mode;
    memcpy(to->offset[to->count], link_offset, sizeof(link_offset));
    to->count++;
}
// the game's delete notes how many were in the list when it ran (in local)
static void game_delete(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    typeof(game_log.call[0]) *call = log_add(Deleted);
    call->arguments[0] = a, call->arguments[1] = b, call->arguments[2] = c, call->arguments[3] = d;
    call->arguments[4] = e, call->arguments[5] = f;
    call->local = list_of(game_cgs)->count;
}
static void game_force_delete(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    typeof(game_log.call[0]) *call = log_add(Force_Deleted);
    call->arguments[0] = a, call->arguments[1] = b, call->arguments[2] = c, call->arguments[3] = d;
    call->arguments[4] = e, call->arguments[5] = f;
}

// the method's arguments as the game gives them: the entity's number low in the second, the local client high in the
// third (with something in the other halves)
static void link_entity(int local, int32_t entnum, uint32_t mode, float x, float y, float z) {
    link_mode = mode, link_offset[0] = x, link_offset[1] = y, link_offset[2] = z;
    if (setjmp(script_error) == 0)
        fake_CScr_LinkToCamera(1, (uint32_t)entnum | 0x500000000ULL, (uint64_t)local << 32 | 0x77, 4, 5, 6);
}
static void delete_entity(int local, int32_t entnum) {
    fake_CScr_Delete(1, (uint32_t)entnum | 0x500000000ULL, (uint64_t)local << 32 | 0x77, 4, 5, 6);
}
static void force_delete_entity(int local, int32_t entnum) {
    fake_CScr_ForceDelete(1, (uint32_t)entnum | 0x500000000ULL, (uint64_t)local << 32 | 0x77, 4, 5, 6);
}
// the view of a local client: its angles, origin and axis, and the time of the frame
static void set_view(int local, float pitch, float yaw, float roll, float x, float y, float z, int32_t time) {
    float angles[3] = { pitch, yaw, roll }, origin[3] = { x, y, z }, axis[9];
    for (int k = 0; k < 9; k++)
        axis[k] = (float)(k + 1) * 0.5f;
    memcpy(cg_of(local) + Cg_View_Angles, angles, sizeof(angles));
    memcpy(cg_of(local) + Cg_View_Origin, origin, sizeof(origin));
    memcpy(cg_of(local) + Cg_View_Axis, axis, sizeof(axis));
    memcpy(cg_of(local) + Cg_Time, &time, sizeof(time));
}
static void set_cent(int which, uint8_t local, int32_t entnum, float pitch, float yaw, float roll) {
    uint8_t *cent = game_cents[which];
    float origin[3] = { 100.0f * (float)which, 7.0f, -3.0f }, angles[3] = { pitch, yaw, roll };
    cent[Cent_Local_Client] = local;
    memcpy(cent + Cent_Entnum, &entnum, sizeof(entnum));
    memcpy(cent + Cent_Origin, origin, sizeof(origin));
    memcpy(cent + Cent_Angles, angles, sizeof(angles));
}
// the game works out a view: its own AnglesToAxis, then the detour behind it
static void view(int local) {
    detour_registers_t registers;
    memset(&registers, 0x5a, sizeof(registers));
    registers.r12 = (uint64_t)cg_of(local);
    detour_registers_t before = registers;
    handler_View_Axis(&registers);
    CHECK(memcmp(&before, &registers, sizeof(registers)) == 0); // no register of the game's is changed
}
static float origin_of(int which, int k) {
    float value;
    memcpy(&value, game_cents[which] + Cent_Origin + 4 * k, sizeof(value));
    return value;
}
static float angle_of(int which, int k) {
    float value;
    memcpy(&value, game_cents[which] + Cent_Angles + 4 * k, sizeof(value));
    return value;
}
static void reset_game(void) {
    memset(&state, 0, sizeof(state));
    memset(game_cgs, 0, 3 * Cg_Size);
    game_local_clients = 1;
    set_view(0, 10.0f, 20.0f, 30.0f, 100.0f, 200.0f, 300.0f, 5000);
    set_view(1, -10.0f, -20.0f, -30.0f, -100.0f, -200.0f, -300.0f, 7000);
    for (int i = 0; i < 8; i++)
        set_cent(i, 0, 1024 + i, 1.0f, 2.0f, 3.0f);
    log_clear();
}

// ---- the rules

static aae_linkcam_list_t made_list(int32_t count, int32_t a, int32_t b, int32_t c, int32_t d) {
    aae_linkcam_list_t made = { .count = count, .entnum = { a, b, c, d } };
    for (int i = 0; i < 4; i++) {
        made.mode[i] = 10 + (uint32_t)i;
        for (int k = 0; k < 3; k++)
            made.offset[i][k] = (float)(100 * i + k);
    }
    return made;
}
// entry i of a list is the one made at j (its number, mode and offset)
static bool entry_is(const aae_linkcam_list_t *l, int i, int32_t entnum, int j) {
    return l->entnum[i] == entnum && l->mode[i] == 10 + (uint32_t)j && l->offset[i][0] == (float)(100 * j) &&
           l->offset[i][1] == (float)(100 * j + 1) && l->offset[i][2] == (float)(100 * j + 2);
}
static bool entry_clear(const aae_linkcam_list_t *l, int i) {
    return l->entnum[i] == 0 && l->mode[i] == 0 && l->offset[i][0] == 0 && l->offset[i][1] == 0 && l->offset[i][2] == 0;
}

static void test_list(void) {
    // where an entity is: within the count, the first of two, and nothing in a list that isn't of 1 to 4
    aae_linkcam_list_t l = made_list(3, 5, 6, 5, 9);
    CHECK(aae_linkcam_find(&l, 5) == 0 && aae_linkcam_find(&l, 6) == 1 && aae_linkcam_find(&l, 9) == -1 &&
          aae_linkcam_find(&l, 7) == -1);
    l.count = 4;
    CHECK(aae_linkcam_find(&l, 9) == 3);
    l.count = 1;
    CHECK(aae_linkcam_find(&l, 5) == 0 && aae_linkcam_find(&l, 6) == -1);
    l.count = 0;
    CHECK(aae_linkcam_find(&l, 5) == -1);
    l.count = 5;
    CHECK(aae_linkcam_find(&l, 5) == -1);
    l.count = -1;
    CHECK(aae_linkcam_find(&l, 5) == -1);

    // taken out: the others move up in their order, with their modes and offsets, the end is cleared
    l = made_list(4, 5, 6, 7, 8);
    CHECK(aae_linkcam_remove(&l, 6) == 1 && l.count == 3);
    CHECK(entry_is(&l, 0, 5, 0) && entry_is(&l, 1, 7, 2) && entry_is(&l, 2, 8, 3) && entry_clear(&l, 3));
    l = made_list(4, 5, 6, 7, 8);
    CHECK(aae_linkcam_remove(&l, 5) == 1 && l.count == 3 && entry_is(&l, 0, 6, 1) && entry_is(&l, 1, 7, 2) &&
          entry_is(&l, 2, 8, 3) && entry_clear(&l, 3));
    l = made_list(4, 5, 6, 7, 8);
    CHECK(aae_linkcam_remove(&l, 8) == 1 && l.count == 3 && entry_is(&l, 0, 5, 0) && entry_is(&l, 2, 7, 2) &&
          entry_clear(&l, 3));
    // twice in it: both go
    l = made_list(4, 5, 6, 5, 8);
    CHECK(aae_linkcam_remove(&l, 5) == 2 && l.count == 2 && entry_is(&l, 0, 6, 1) && entry_is(&l, 1, 8, 3) &&
          entry_clear(&l, 2) && entry_clear(&l, 3));
    // the last one: none left, all cleared
    l = made_list(1, 5, 6, 7, 8);
    CHECK(aae_linkcam_remove(&l, 5) == 1 && l.count == 0 && entry_clear(&l, 0) && entry_is(&l, 1, 6, 1) &&
          entry_is(&l, 3, 8, 3));
    // not in it, or behind the count: nothing changes
    aae_linkcam_list_t kept = made_list(2, 5, 6, 7, 8);
    l = kept;
    CHECK(aae_linkcam_remove(&l, 7) == 0 && memcmp(&l, &kept, sizeof(l)) == 0);
    // a list that isn't of 1 to 4 isn't touched
    const int32_t odd[] = { 0, 5, -1, 0x7fffffff };
    for (int i = 0; i < 4; i++) {
        kept = made_list(odd[i], 5, 6, 7, 8);
        l = kept;
        CHECK(aae_linkcam_remove(&l, 5) == 0 && memcmp(&l, &kept, sizeof(l)) == 0);
    }

    // the plugin's copy of the numbers: a list of 1 to 4, else none
    aae_linkcam_ids_t ids = { .count = 3, .entnum = { 1, 2, 3, 4 } };
    l = made_list(2, 5, 6, 7, 8);
    aae_linkcam_note(&ids, &l);
    CHECK(ids.count == 2 && ids.entnum[0] == 5 && ids.entnum[1] == 6);
    CHECK(aae_linkcam_noted(&ids, 5) && aae_linkcam_noted(&ids, 6) && !aae_linkcam_noted(&ids, 3) &&
          !aae_linkcam_noted(&ids, 7));
    l.count = 4;
    aae_linkcam_note(&ids, &l);
    CHECK(ids.count == 4 && aae_linkcam_noted(&ids, 8) && aae_linkcam_noted(&ids, 7));
    for (int i = 0; i < 4; i++) {
        l.count = odd[i];
        aae_linkcam_note(&ids, &l);
        CHECK(ids.count == 0 && !aae_linkcam_noted(&ids, 5));
    }
    l.count = 4;
    aae_linkcam_note(&ids, &l);
    aae_linkcam_note(&ids, NULL);
    CHECK(ids.count == 0 && !aae_linkcam_noted(&ids, 5));
    // no more than four are looked at, whatever the count says
    aae_linkcam_ids_t wide = { .count = 9, .entnum = { 1, 2, 3, 4 } };
    CHECK(aae_linkcam_noted(&wide, 4) && !aae_linkcam_noted(&wide, 0));
}

static bool same3(const float a[3], float x, float y, float z) {
    return a[0] == x && a[1] == y && a[2] == z;
}
static void test_sums(void) {
    const float own[3] = { 1.0f, 2.0f, 3.0f }, view[3] = { 10.0f, 20.0f, 30.0f };
    float angles[3];
    // the view's pitch, yaw or roll, with the axis made from them
    CHECK(aae_linkcam_angles(0, own, view, angles) && same3(angles, 10.0f, 2.0f, 3.0f));
    CHECK(aae_linkcam_angles(1, own, view, angles) && same3(angles, 1.0f, 20.0f, 3.0f));
    CHECK(aae_linkcam_angles(2, own, view, angles) && same3(angles, 1.0f, 2.0f, 30.0f));
    // the view's angles with the view's axis: 3, 4, and every mode the plugin doesn't name
    const uint32_t views[] = { 3, 4, 6, 7, 100, 0x80000000u, 0xffffffffu };
    for (int i = 0; i < 7; i++)
        CHECK(!aae_linkcam_angles(views[i], own, view, angles) && same3(angles, 10.0f, 20.0f, 30.0f));
    // its own with the view's axis
    CHECK(!aae_linkcam_angles(5, own, view, angles) && same3(angles, 1.0f, 2.0f, 3.0f));

    // the view's origin and the offset along the axis's three directions
    const float axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, turned[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };
    float origin[3];
    aae_linkcam_origin((float[3]){ 1, 2, 3 }, (float[3]){ 10, 20, 30 }, axis, origin);
    CHECK(same3(origin, 11.0f, 22.0f, 33.0f));
    aae_linkcam_origin((float[3]){ 1, 2, 3 }, (float[3]){ 10, 20, 30 }, turned, origin);
    CHECK(same3(origin, 8.0f, 21.0f, 33.0f));
    // in the plugin's order: ((x along the first + origin) + y along the second) + z along the third, each rounded.
    // 1 + 2^24 is 2^24 in floats, so the grouping shows: each component here is 0 that way, and 1 or 2 any other
    const float big = 16777216.0f;
    aae_linkcam_origin((float[3]){ 1, 1, -1 }, (float[3]){ big, 1, big },
                       (float[9]){ 1, big, 1, 1, 1, 1, big, big, big }, origin);
    CHECK(origin[0] == 0.0f && origin[1] == 0.0f && origin[2] == 0.0f);
    // each part of the offset along its own row of the axis: x along the first, y the second, z the third
    aae_linkcam_origin((float[3]){ 1, 10, 100 }, (float[3]){ 0, 0, 0 }, (float[9]){ 1, 2, 3, 4, 5, 6, 7, 8, 9 },
                       origin);
    CHECK(same3(origin, 741.0f, 852.0f, 963.0f));

    // moved: only numbers, and only when something differs bit for bit
    const float here[3] = { 1, 2, 3 }, there[3] = { 4, 5, 6 };
    CHECK(!aae_linkcam_moves(here, there, here, there));
    CHECK(aae_linkcam_moves((float[3]){ 1, 2, 3.5f }, there, here, there) &&
          aae_linkcam_moves(here, (float[3]){ 4, 5.5f, 6 }, here, there));
    for (int k = 0; k < 3; k++) {
        float a[3] = { 1, 2, 3 }, b[3] = { 4, 5, 6 };
        a[k] = NAN;
        CHECK(!aae_linkcam_moves(a, there, here, there));
        a[k] = INFINITY;
        CHECK(!aae_linkcam_moves(a, there, here, there));
        a[k] = -INFINITY;
        CHECK(!aae_linkcam_moves(a, there, here, there));
        b[k] = NAN;
        CHECK(!aae_linkcam_moves(here, b, here, there));
        b[k] = -INFINITY;
        CHECK(!aae_linkcam_moves(here, b, here, there));
        // the biggest number is still one
        a[k] = 3.4028235e38f;
        CHECK(aae_linkcam_moves(a, there, here, there));
    }
    CHECK(aae_linkcam_moves((float[3]){ -0.0f, 2, 3 }, there, (float[3]){ 0.0f, 2, 3 }, there) &&
          aae_linkcam_moves(here, (float[3]){ 4, 5, -0.0f }, here, (float[3]){ 4, 5, 0.0f }));
    // where it is now may be anything
    CHECK(aae_linkcam_moves(here, there, (float[3]){ NAN, 2, 3 }, there) &&
          aae_linkcam_moves(here, there, here, (float[3]){ 4, 5, INFINITY }));
}

static void test_hold(void) {
    aae_linkcam_held_t held = { 0 };
    uint8_t cents[6][8], lighting[32], extra[48], other[32];
    const uint8_t *extra_at = extra, *none = NULL;
    const uint8_t cg[4] = { 0 };
    for (int i = 0; i < 32; i++)
        lighting[i] = (uint8_t)(i + 1), other[i] = (uint8_t)(200 + i);
    for (int i = 0; i < 48; i++)
        extra[i] = (uint8_t)(100 + i);
    lighting[0] = Lighting_Has_Extra | 1;
    memcpy(lighting + Lighting_Extra, &extra_at, sizeof(extra_at));
    other[0] = 2;
    memcpy(other + Lighting_Extra, &extra_at, sizeof(extra_at));
    // everything the drawing function was given, with copies of how it is lit and what more that points to
    aae_linkcam_hold(&held, (void *)0x10, cg, cents[0], 1024, 0xabc, lighting, 5000);
    aae_linkcam_slot_t *slot = &held.slot[0];
    CHECK(held.count == 1 && slot->object == (void *)0x10 && slot->cg == cg && slot->cent == cents[0] &&
          slot->entnum == 1024 && slot->flags == 0xabc && slot->time == 5000 && slot->had_lighting &&
          slot->had_extra && memcmp(slot->lighting, lighting, 32) == 0 && memcmp(slot->extra, extra, 48) == 0);
    // the same entity again: its slot, anew
    aae_linkcam_hold(&held, (void *)0x20, cg, cents[0], 1030, 0xdef, other, 5016);
    CHECK(held.count == 1 && slot->object == (void *)0x20 && slot->entnum == 1030 && slot->flags == 0xdef &&
          slot->time == 5016 && slot->had_lighting && !slot->had_extra && memcmp(slot->lighting, other, 32) == 0 &&
          memcmp(slot->extra, extra, 48) == 0);
    // without anything for how it is lit: none, and the copies stay as they were
    aae_linkcam_hold(&held, (void *)0x20, cg, cents[0], 1030, 0xdef, NULL, 5016);
    CHECK(held.count == 1 && !slot->had_lighting && !slot->had_extra && memcmp(slot->lighting, other, 32) == 0);
    // the flag for more without a pointer, or a pointer without the flag: no more
    memcpy(lighting + Lighting_Extra, &none, sizeof(none));
    aae_linkcam_hold(&held, (void *)0x30, cg, cents[1], 1025, 0, lighting, 5000);
    CHECK(held.count == 2 && held.slot[1].had_lighting && !held.slot[1].had_extra);
    // others in the next slots, up to four; a fifth isn't kept
    aae_linkcam_hold(&held, (void *)0x40, cg, cents[2], 1026, 0, NULL, 5000);
    aae_linkcam_hold(&held, (void *)0x50, cg, cents[3], 1027, 0, NULL, 5000);
    CHECK(held.count == 4 && held.slot[3].cent == cents[3]);
    aae_linkcam_slot_t before[4];
    memcpy(before, held.slot, sizeof(before));
    aae_linkcam_hold(&held, (void *)0x60, cg, cents[4], 1028, 0, lighting, 5000);
    CHECK(held.count == 4 && memcmp(before, held.slot, sizeof(before)) == 0);
    // one of the four again still is
    aae_linkcam_hold(&held, (void *)0x70, cg, cents[2], 1026, 7, NULL, 5000);
    CHECK(held.count == 4 && held.slot[2].object == (void *)0x70 && held.slot[2].flags == 7);
}

// ---- the hooks

static void test_hooks(void) {
    reset_game();
    set_mod(true);
    // a gesture: linked with the view's angles at 5 forward, 2 left and 3 down
    link_entity(0, 1024, 4, 5.0f, 2.0f, -3.0f);
    CHECK(list(0)->count == 1 && list(0)->entnum[0] == 1024 && state.cg == cg_of(0) && state.noted.count == 1 &&
          state.noted.entnum[0] == 1024);
    typeof(game_log.call[0]) *call = log_find(Linked, 0);
    CHECK(call != NULL && call->arguments[0] == 1 && call->arguments[1] == (1024 | 0x500000000ULL) &&
          call->arguments[2] == 0x77 && call->arguments[3] == 4 && call->arguments[4] == 5 && call->arguments[5] == 6);
    // a frame: the linked one is held back, another one is drawn at once
    log_clear();
    uint8_t lighting[32] = { Lighting_Has_Extra | 2, 9, 8 }, extra[48] = { 77, 66 };
    const uint8_t *extra_at = extra;
    memcpy(lighting + Lighting_Extra, &extra_at, sizeof(extra_at));
    fake_AddDObjToScene((void *)0x51, game_cents[0], 1024, 0x123, lighting);
    fake_AddDObjToScene((void *)0x52, game_cents[1], 1025, 0x456, NULL);
    CHECK(game_log.count == 1 && log_find(Drawn, 0)->cent == game_cents[1] && log_find(Drawn, 0)->entnum == 1025 &&
          log_find(Drawn, 0)->flags == 0x456 && log_find(Drawn, 0)->object == (void *)0x52 &&
          log_find(Drawn, 0)->lighting == NULL);
    CHECK(state.held.count == 1);
    // the view: it is put there (axis rows 0.5 1 1.5, 2 2.5 3, 3.5 4 4.5) with the view's angles, and drawn
    log_clear();
    view(0);
    CHECK(state.held.count == 0 && !state.placing && log_count(Axis) == 0);
    call = log_find(Drawn, 0);
    CHECK(call != NULL && call->cent == game_cents[0] && call->entnum == 1024 && call->flags == 0x123 &&
          call->object == (void *)0x51);
    CHECK(same3(call->origin, 5 * 0.5f + 100 + 2 * 2.0f + -3 * 3.5f, 5 * 1.0f + 200 + 2 * 2.5f + -3 * 4.0f,
                5 * 1.5f + 300 + 2 * 3.0f + -3 * 4.5f) &&
          same3(call->angles, 10, 20, 30));
    // drawn with a copy of how it was lit, its pointer to a copy of the more
    CHECK(call->lighting != lighting && memcmp(call->lighting_copy, lighting, Lighting_Extra) == 0 &&
          call->had_extra && memcmp(call->extra_copy, extra, sizeof(extra)) == 0);
    const uint8_t *pointed;
    memcpy(&pointed, call->lighting_copy + Lighting_Extra, sizeof(pointed));
    CHECK(pointed == state.held.slot[0].extra);
    CHECK(log_count(Pose) == 1 && log_find(Pose, 0)->entnum == 1024 && log_find(Pose, 0)->local == 0);
    // told before it is drawn
    CHECK(game_log.call[0].kind == Pose && game_log.call[1].kind == Drawn);
    // the same frame once more: already there, so the game isn't told again
    log_clear();
    fake_AddDObjToScene((void *)0x51, game_cents[0], 1024, 0x123, NULL);
    view(0);
    CHECK(log_count(Drawn) == 1 && log_count(Pose) == 0 && log_find(Drawn, 0)->lighting == NULL);
    // the view turns: it follows
    set_view(0, 15.0f, 25.0f, 35.0f, 100.0f, 200.0f, 310.0f, 5016);
    log_clear();
    fake_AddDObjToScene((void *)0x51, game_cents[0], 1024, 0x123, NULL);
    view(0);
    CHECK(log_count(Pose) == 1 && angle_of(0, 0) == 15.0f && angle_of(0, 1) == 25.0f && angle_of(0, 2) == 35.0f &&
          origin_of(0, 2) == 5 * 1.5f + 310 + 2 * 3.0f + -3 * 4.5f);
    // the entity is deleted: off the list, and the game's own delete is called with what it was given
    log_clear();
    delete_entity(0, 1024);
    CHECK(list(0)->count == 0 && state.noted.count == 0 && log_count(Deleted) == 1);
    call = log_find(Deleted, 0);
    CHECK(call->local == 0); // off the list before the game's own delete ran
    CHECK(call->arguments[0] == 1 && call->arguments[1] == (1024 | 0x500000000ULL) && call->arguments[2] == 0x77 &&
          call->arguments[3] == 4 && call->arguments[4] == 5 && call->arguments[5] == 6);
    // and drawn at once again (where it was left)
    log_clear();
    fake_AddDObjToScene((void *)0x51, game_cents[0], 1024, 0x123, NULL);
    CHECK(log_count(Drawn) == 1 && state.held.count == 0);

    // more than four gestures: each one's entity is taken off when it is deleted, so the list never fills
    for (int i = 0; i < 10; i++) {
        link_entity(0, 1025 + i % 3, 4, 1, 1, 1);
        delete_entity(0, 1025 + i % 3);
    }
    CHECK(list(0)->count == 0);
    // with four there, a fifth is the game's script error (nothing behind it runs), and the list stays
    for (int i = 0; i < 4; i++)
        link_entity(0, 1025 + i, 4, 1, 1, 1);
    link_entity(0, 1029, 4, 1, 1, 1);
    CHECK(list(0)->count == 4 && state.noted.count == 4 && !aae_linkcam_noted(&state.noted, 1029));
    // forcedelete takes off as delete does
    log_clear();
    force_delete_entity(0, 1026);
    CHECK(list(0)->count == 3 && list(0)->entnum[0] == 1025 && list(0)->entnum[1] == 1027 &&
          list(0)->entnum[2] == 1028 && state.noted.count == 3 && !aae_linkcam_noted(&state.noted, 1026) &&
          log_count(Force_Deleted) == 1 && log_find(Force_Deleted, 0)->arguments[1] == (1026 | 0x500000000ULL));
    // deleting something not linked changes nothing, and the game's delete still runs
    log_clear();
    aae_linkcam_ids_t noted = state.noted;
    delete_entity(0, 999);
    CHECK(list(0)->count == 3 && memcmp(&noted, &state.noted, sizeof(noted)) == 0 && log_count(Deleted) == 1);
}

// the modes, with what the axis is made from and what the entity gets
static void test_modes(void) {
    reset_game();
    set_mod(true);
    const struct {
        uint32_t mode;
        bool axis_made;
        float angles[3];
    } cases[] = { { 0, true, { 10, 2, 3 } },  { 1, true, { 1, 20, 3 } },     { 2, true, { 1, 2, 30 } },
                  { 3, false, { 10, 20, 30 } }, { 4, false, { 10, 20, 30 } }, { 5, false, { 1, 2, 3 } },
                  { 6, false, { 10, 20, 30 } }, { 0xffffffffu, false, { 10, 20, 30 } } };
    for (int i = 0; i < 8; i++) {
        reset_game();
        link_entity(0, 1026, cases[i].mode, 1.0f, 2.0f, 3.0f);
        log_clear();
        fake_AddDObjToScene((void *)1, game_cents[2], 1026, 0, NULL);
        view(0);
        CHECK(log_count(Axis) == (cases[i].axis_made ? 1 : 0));
        float axis[9];
        if (cases[i].axis_made) {
            CHECK(same3(log_find(Axis, 0)->angles, cases[i].angles[0], cases[i].angles[1], cases[i].angles[2]));
            for (int k = 0; k < 9; k++)
                axis[k] = cases[i].angles[k % 3] + (float)(k + 1) * 1000.0f;
        } else {
            for (int k = 0; k < 9; k++)
                axis[k] = (float)(k + 1) * 0.5f;
        }
        float expected[3];
        aae_linkcam_origin((float[3]){ 1, 2, 3 }, (float[3]){ 100, 200, 300 }, axis, expected);
        CHECK(log_count(Drawn) == 1 && same3(log_find(Drawn, 0)->origin, expected[0], expected[1], expected[2]) &&
              same3(log_find(Drawn, 0)->angles, cases[i].angles[0], cases[i].angles[1], cases[i].angles[2]));
    }
    // the local client the game is told of: the entity's own, up to 3, otherwise 0
    const uint8_t locals[] = { 0, 1, 3, 4, 255 };
    const int told[] = { 0, 1, 3, 0, 0 };
    for (int i = 0; i < 5; i++) {
        reset_game();
        set_cent(2, locals[i], 1026, 1, 2, 3);
        link_entity(0, 1026, 4, 1, 2, 3);
        log_clear();
        fake_AddDObjToScene((void *)1, game_cents[2], 1026, 0, NULL);
        view(0);
        CHECK(log_count(Pose) == 1 && log_find(Pose, 0)->local == told[i] && log_find(Pose, 0)->entnum == 1026);
    }
    // nowhere to put it (an origin that isn't a number): drawn where it is, the game told of nothing
    reset_game();
    set_view(0, 10.0f, 20.0f, 30.0f, NAN, 200.0f, 300.0f, 5000);
    link_entity(0, 1026, 4, 1, 2, 3);
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[2], 1026, 0, NULL);
    view(0);
    CHECK(log_count(Drawn) == 1 && log_count(Pose) == 0 && origin_of(2, 0) == 200.0f && angle_of(2, 0) == 1.0f);
    reset_game();
    set_view(0, INFINITY, 20.0f, 30.0f, 100.0f, 200.0f, 300.0f, 5000);
    link_entity(0, 1026, 4, 1, 2, 3);
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[2], 1026, 0, NULL);
    view(0);
    CHECK(log_count(Drawn) == 1 && log_count(Pose) == 0 && angle_of(2, 0) == 1.0f);
}

// held back, but not drawn: the frame or the view isn't the one it was held back for, or the entity changed
static void test_held(void) {
    // the time of the frame changed before the view
    reset_game();
    set_mod(true);
    link_entity(0, 1024, 4, 1, 2, 3);
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    set_view(0, 10.0f, 20.0f, 30.0f, 100.0f, 200.0f, 300.0f, 5016);
    view(0);
    CHECK(log_count(Drawn) == 0 && state.held.count == 0 && log_count(Pose) == 0);
    // held for another local client's view
    reset_game();
    game_local_clients = 2;
    link_entity(1, 1024, 4, 1, 2, 3);
    CHECK(list(1)->count == 1 && list(0)->count == 0 && state.cg == cg_of(1));
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(state.held.count == 1 && state.held.slot[0].cg == cg_of(1) && state.held.slot[0].time == 7000);
    view(0);
    CHECK(log_count(Drawn) == 0 && state.held.count == 0 && state.cg == cg_of(0) && state.noted.count == 0);
    // and drawn in its own: its time, its list
    link_entity(1, 1024, 4, 1, 2, 3);
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    view(1);
    CHECK(log_count(Drawn) == 1 && log_count(Pose) == 1 && origin_of(0, 0) == 1 * 0.5f + -100 + 2 * 2.0f + 3 * 3.5f);
    // its number changed, or there was no model
    reset_game();
    link_entity(0, 1024, 4, 1, 2, 3);
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    set_cent(0, 0, 1031, 1, 2, 3);
    view(0);
    CHECK(log_count(Drawn) == 0 && state.held.count == 0);
    reset_game();
    link_entity(0, 1024, 4, 1, 2, 3);
    log_clear();
    fake_AddDObjToScene(NULL, game_cents[0], 1024, 0, NULL);
    CHECK(state.held.count == 1);
    view(0);
    CHECK(log_count(Drawn) == 0 && state.held.count == 0);
    // all four places taken: a fifth one held back isn't drawn in that frame at all
    reset_game();
    link_entity(0, 1024, 4, 1, 2, 3);
    log_clear();
    for (int i = 0; i < 5; i++) {
        set_cent(i, 0, 1024, 1, 2, 3);
        fake_AddDObjToScene((void *)1, game_cents[i], 1024, (uint32_t)i, NULL);
    }
    CHECK(state.held.count == 4 && log_count(Drawn) == 0);
    view(0);
    CHECK(log_count(Drawn) == 4 && log_find(Drawn, 3)->flags == 3);
    // what isn't held back: not linked (by the copy), another entity than the number says, none at all
    reset_game();
    link_entity(0, 1024, 4, 1, 2, 3);
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[1], 1024, 0, NULL);
    fake_AddDObjToScene((void *)1, NULL, 1024, 0, NULL);
    fake_AddDObjToScene((void *)1, game_cents[0], 1025, 0, NULL);
    CHECK(log_count(Drawn) == 3 && state.held.count == 0);
    // linked by the copy but not (any more) in the list: drawn at once
    list(0)->count = 0;
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 1 && state.held.count == 0);
    // a list that isn't of 1 to 4 counts as none
    list(0)->count = 5;
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 2 && state.held.count == 0);
    // the copy is taken anew at every view: in the list since, linked from then on
    reset_game();
    list(0)->count = 1, list(0)->entnum[0] = 1024, list(0)->mode[0] = 4;
    state.cg = cg_of(0);
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 1);
    view(0);
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 0 && state.held.count == 1);
    // no cg_t to go by yet: drawn at once
    reset_game();
    state.noted.count = 1, state.noted.entnum[0] = 1024;
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 1 && state.held.count == 0);
}

// while the held-back ones are drawn, every entity drawn is put at the view if it is linked
static void test_placing(void) {
    reset_game();
    set_mod(true);
    link_entity(0, 1024, 4, 1, 2, 3);
    state.placing = true;
    fake_AddDObjToScene((void *)1, game_cents[1], 1025, 0, NULL);
    CHECK(log_count(Drawn) == 1 && log_count(Pose) == 0 && origin_of(1, 0) == 100.0f);
    // it is placed by its own number, told by the one it was drawn with
    set_cent(1, 0, 1024, 1, 2, 3);
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[1], 1099, 0, NULL);
    CHECK(log_count(Drawn) == 1 && log_count(Pose) == 1 && log_find(Pose, 0)->entnum == 1099 &&
          angle_of(1, 1) == 20.0f);
    state.placing = false;
    // a view that comes while they are drawn does nothing more
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    state.placing = true;
    view(0);
    CHECK(state.held.count == 1 && log_count(Drawn) == 0);
    state.placing = false;
    view(0);
    CHECK(state.held.count == 0 && log_count(Drawn) == 1);
}

// without the mod and its plugin every hook only hands on to the game
static void test_without_the_mod(void) {
    reset_game();
    set_mod(false);
    link_entity(0, 1024, 4, 1, 2, 3);
    CHECK(list(0)->count == 1 && log_count(Linked) == 1 && state.cg == NULL && state.noted.count == 0);
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 1 && state.held.count == 0);
    delete_entity(0, 1024);
    force_delete_entity(0, 1024);
    CHECK(list(0)->count == 1 && log_count(Deleted) == 1 && log_count(Force_Deleted) == 1);
    // what was kept from before is let go
    state.noted.count = 1, state.noted.entnum[0] = 1024, state.held.count = 1, state.cg = cg_of(0);
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 2 && state.held.count == 1);
    view(0);
    CHECK(state.noted.count == 0 && state.held.count == 0 && log_count(Drawn) == 2);
    delete_entity(0, 1024);
    CHECK(list(0)->count == 1);
    // and with it again, as before
    set_mod(true);
    delete_entity(0, 1024);
    CHECK(list(0)->count == 0);
}

// the cg_t a method is for, as the game's methods find it
static void test_methods(void) {
    reset_game();
    set_mod(true);
    game_local_clients = 2;
    CHECK(cg_of_method(0) == cg_of(0) && cg_of_method(1ULL << 32) == cg_of(1) &&
          cg_of_method(1ULL << 32 | 0xffffffff) == cg_of(1));
    CHECK(cg_of_method(2ULL << 32) == NULL && cg_of_method(0xffffffffULL << 32) != NULL);
    game_local_clients = 1;
    CHECK(cg_of_method(1ULL << 32) == NULL && cg_of_method(0) == cg_of(0));
    uint8_t *all = game_cgs;
    game_cgs = NULL;
    CHECK(cg_of_method(0) == NULL);
    game_local_clients = 2;
    CHECK(cg_of_method(1ULL << 32) == NULL);
    game_cgs = all;
    // delete goes by the cg_t of the last link, whichever local client it is for
    game_local_clients = 2;
    link_entity(1, 1024, 4, 1, 2, 3);
    link_entity(0, 1025, 4, 1, 2, 3);
    delete_entity(1, 1024);
    CHECK(list(1)->count == 1 && list(0)->count == 1);
    delete_entity(1, 1025);
    CHECK(list(0)->count == 0 && state.noted.count == 0);
}

// The game makes its cg_t array anew when a map is left, perhaps elsewhere. What was kept of the old game's then
// points at memory the game has given back: nothing is read from it or written to it again.
static void test_the_next_game(void) {
    reset_game();
    set_mod(true);
    link_entity(0, 1024, 4, 1, 2, 3);
    CHECK(list(0)->count == 1 && state.cg == cg_of(0) && state.noted.count == 1);
    uint8_t *old = game_cgs, *next = calloc(3, Cg_Size);
    aae_linkcam_list_t old_list = *list_of(old);
    // (the hand was being held back for the frame's view when the game was left)
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(state.held.count == 1);
    // the menu's or the next map's array is another one; where the old one was, the old list still stands
    game_cgs = next;
    log_clear();
    delete_entity(0, 1024);
    CHECK(memcmp(list_of(old), &old_list, sizeof(old_list)) == 0 && log_count(Deleted) == 1);
    CHECK(state.cg == NULL && state.noted.count == 0 && state.held.count == 0);
    // an entity that has the old hand's number is drawn at once, where it is, and nothing waits for a view
    state.cg = old;
    aae_linkcam_note(&state.noted, list_of(old));
    log_clear();
    fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
    CHECK(log_count(Drawn) == 1 && log_count(Pose) == 0 && state.held.count == 0 && state.cg == NULL &&
          state.noted.count == 0 && memcmp(list_of(old), &old_list, sizeof(old_list)) == 0);
    // forcedelete, and no array at all (between two maps)
    state.cg = old;
    force_delete_entity(0, 1024);
    CHECK(memcmp(list_of(old), &old_list, sizeof(old_list)) == 0 && state.cg == NULL);
    state.cg = old;
    uint8_t *no_array = NULL;
    cg_clients = &no_array;
    delete_entity(0, 1024);
    CHECK(memcmp(list_of(old), &old_list, sizeof(old_list)) == 0 && state.cg == NULL);
    cg_clients = &game_cgs;
    // Only a cg_t of the array counts: not one behind the local clients there are, not a place inside one, not the
    // byte before the array. The second local client's does when there are two.
    game_cgs = old;
    state.cg = old + Cg_Size;
    CHECK(kept_cg() == NULL);
    game_local_clients = 2;
    state.cg = old + Cg_Size;
    CHECK(kept_cg() == old + Cg_Size);
    state.cg = old + 2 * Cg_Size;
    CHECK(kept_cg() == NULL);
    state.cg = old + 8;
    CHECK(kept_cg() == NULL);
    state.cg = old - 1;
    CHECK(kept_cg() == NULL);
    state.cg = old;
    CHECK(kept_cg() == old);
    game_local_clients = 0;
    CHECK(kept_cg() == NULL);
    state.cg = old;
    game_local_clients = -1;
    CHECK(kept_cg() == NULL);
    // in the same game everything goes on as before: the delete takes the entity off the list
    game_local_clients = 1;
    state.cg = old;
    aae_linkcam_note(&state.noted, list_of(old));
    delete_entity(0, 1024);
    CHECK(list(0)->count == 0 && state.noted.count == 0 && state.cg == old);
    free(next);
}

// ---- the debug lines, which the game checks go by (wip/agents/ltc/gamecheck): what a gesture prints

static char debug_text[8192];
// runs a gesture (linked, drawn twice at the view, deleted) with the debug lines on, and keeps what they printed
static void debug_gesture(bool third_person, bool another_thread) {
    char path[4200];
    snprintf(path, sizeof(path), "%s/debug.txt", mod_folder);
    fflush(stdout);
    int kept = dup(1), file = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    dup2(file, 1);
    debug = 1;
    memset(&told, 0, sizeof(told));
    reset_game();
    set_mod(true);
    cg_of(0)[Cg_Third_Person] = third_person;
    view(0);
    if (another_thread)
        told.frame_thread++;
    for (int i = 0; i < 2; i++) {
        link_entity(0, 1024, 4, 1, 2, 3);
        fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
        view(0);
        fake_AddDObjToScene((void *)1, game_cents[0], 1024, 0, NULL);
        view(0);
        delete_entity(0, 1024);
    }
    debug = 0;
    fflush(stdout);
    dup2(kept, 1);
    close(kept);
    close(file);
    memset(debug_text, 0, sizeof(debug_text));
    FILE *in = fopen(path, "r");
    if (in != NULL) {
        fread(debug_text, 1, sizeof(debug_text) - 1, in);
        fclose(in);
    }
}
static void test_debug_lines(void) {
    debug_gesture(false, false);
    CHECK(strstr(debug_text, "gestures: entity 1024 linked to the camera, link 1 since the start (drawn at the view "
                             "0 times since the last), 1 linked now: 1024 (mode 4, offset 1.0 2.0 3.0) (thread ") !=
          NULL);
    CHECK(strstr(debug_text, "linked to the camera, link 2 since the start") != NULL);
    CHECK(strstr(debug_text, "gestures: entity 1024 drawn at the view (mode 4): at ") != NULL);
    CHECK(strstr(debug_text, "gestures: entity 1024 deleted and taken off the list (drawn at the view 2 times since "
                             "the last), 0 linked now: (thread ") != NULL);
    CHECK(strstr(debug_text, "third person") == NULL && strstr(debug_text, "NOT the one") == NULL &&
          strstr(debug_text, "where it was already") != NULL);
    debug_gesture(true, true);
    CHECK(strstr(debug_text, "angles 10.0 20.0 30.0, a third person view (thread ") != NULL);
    CHECK(strstr(debug_text, "NOT the one the views are worked out on") != NULL);
}

// ---- the same cases as the plugin's own functions were given (see linkcam_cases.h), and what that gave

#define Plugin_Lines 210417
#define Plugin_Checksum 0xfb0d3a374af843a8ULL
static struct {
    uint64_t checksum;
    int lines;
} cases_out;
static void cases_byte(uint8_t byte) {
    cases_out.checksum = (cases_out.checksum ^ byte) * 0x100000001b3ULL;
}
static void cases_line(const char *line) {
    for (; *line != '\0'; line++)
        cases_byte((uint8_t)*line);
    cases_byte('\n');
    cases_out.lines++;
}
#include "linkcam_cases.h"
#include "linkcam_side.h"
static void test_like_the_plugin(void) {
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    side_cases_run();
    CHECK(cases_out.lines == Plugin_Lines);
    CHECK(cases_out.checksum == Plugin_Checksum);
    if (cases_out.checksum != Plugin_Checksum || cases_out.lines != Plugin_Lines)
        printf("  the cases came out as %d lines, %#018llx: wip/agents/ltc/compare/compare.sh shows where they differ "
               "from the plugin's\n", cases_out.lines, (unsigned long long)cases_out.checksum);
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    hooks = 0;
    aae_linkcam_init(base, 0);
    CHECK(hooks == 0);
    // not from the pre-patched executable: nothing at all (entities held back would never be drawn)
    have_stubs = false;
    aae_linkcam_init(base, 1);
    CHECK(hooks == 0);
    have_stubs = true;
    aae_linkcam_init(base, 1);
    CHECK(hooks == 5);
    CHECK(was_hooked(ADDR_AddDObjToScene, (void *)fake_AddDObjToScene, (void **)&orig_AddDObjToScene) &&
          was_hooked(ADDR_CScr_LinkToCamera, (void *)fake_CScr_LinkToCamera, (void **)&orig_CScr_LinkToCamera) &&
          was_hooked(ADDR_CScr_Delete, (void *)fake_CScr_Delete, (void **)&orig_CScr_Delete) &&
          was_hooked(ADDR_CScr_ForceDelete, (void *)fake_CScr_ForceDelete, (void **)&orig_CScr_ForceDelete) &&
          was_hooked(ADDR_CG_CalcViewValues_ViewAxis, (void *)handler_View_Axis, &orig_View_Axis));
    CHECK(AnglesToAxis == (void *)(base + ADDR_AnglesToAxis) &&
          CG_EntityPoseChanged == (void *)(base + ADDR_CG_EntityPoseChanged) &&
          cg_clients == (void *)(base + ADDR_cgArray) &&
          local_client_count == (void *)(base + ADDR_LOCAL_CLIENT_COUNT));
    // the places, as they were read from the game's code
    CHECK(ADDR_AddDObjToScene == 0xe0f8b8 && ADDR_CScr_LinkToCamera == 0xef8f94 && ADDR_CScr_Delete == 0xef8174 &&
          ADDR_CScr_ForceDelete == 0xef825d && ADDR_CG_CalcViewValues_ViewAxis == 0xfbd911 &&
          ADDR_AnglesToAxis == 0x1352ba1 && ADDR_CG_EntityPoseChanged == 0x104b9ab);
    CHECK(Cg_Size == 0x342a80 && Cg_Camera_List == 0x3272f4 && Cg_Time == 0x11a8ac && Cg_View_Origin == 0x131dc8 &&
          Cg_View_Axis == 0x131de4 && Cg_View_Angles == 0x2d8930 && Cent_Local_Client == 2 && Cent_Origin == 0x40 &&
          Cent_Angles == 0x4c && Cent_Entnum == 0x3f8 && Lighting_Has_Extra == 8 && Lighting_Extra == 0x18 &&
          Max_Local_Client == 3);
    CHECK(offsetof(aae_linkcam_list_t, entnum) == 0x3272f8 - 0x3272f4 &&
          offsetof(aae_linkcam_list_t, mode) == 0x327308 - 0x3272f4 &&
          offsetof(aae_linkcam_list_t, offset) == 0x327318 - 0x3272f4);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-linkcam-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/2631943123", template);
    char command[4096 + 64];
    snprintf(command, sizeof(command), "mkdir -p '%s/T7Overcharged/plugins'", mod_folder);
    if (system(command) != 0)
        return 2;
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    plugin_now_ms = test_plugin_now;
    game_cgs = calloc(3, Cg_Size);

    test_init();
    loaded_mod = loaded_name;
    cg_clients = &game_cgs;
    local_client_count = &game_local_clients;
    AnglesToAxis = game_axis;
    CG_EntityPoseChanged = game_pose;
    orig_AddDObjToScene = game_draw;
    orig_CScr_LinkToCamera = game_link;
    orig_CScr_Delete = game_delete;
    orig_CScr_ForceDelete = game_force_delete;
    test_list();
    test_sums();
    test_hold();
    test_hooks();
    test_modes();
    test_held();
    test_placing();
    test_without_the_mod();
    test_methods();
    test_the_next_game();
    test_debug_lines();
    test_like_the_plugin();

    free(game_cgs);
    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_linkcam: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_linkcam: all passed\n");
    return 0;
}
