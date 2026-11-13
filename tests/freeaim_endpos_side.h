// BO3MacFix's side of freeaim_endpos_cases.h: aae_freeaim_match for queued shots, and the client's copy of a shot
// through client_shot with a made-up game of its own (the local clients' cg_t, the dvars, the models, the fire
// function's frame), which it puts back as it found it after each case. Included by test_aae_freeaim.c (which checks
// the plugin's output by its line count and checksum) and by the program that compares with the plugin's own code.
// Include aae_freeaim.c and freeaim_endpos_cases.h first, and have the mod with its plugin count as loaded.

// what queued_shot does with what it found: the three points written
static int side_match(endpos_case_ring *ring, uint64_t now, float origin[3], float start[3], float muzzle[3]) {
    aae_freeaim_tracers_t kept;
    memset(&kept, 0, sizeof(kept));
    for (int i = 0; i < AAE_FREEAIM_SHOTS; i++) {
        memcpy(kept.shots[i].start, ring->shots[i].start, sizeof(kept.shots[i].start));
        memcpy(kept.shots[i].flash, ring->shots[i].flash, sizeof(kept.shots[i].flash));
        memcpy(kept.shots[i].target, ring->shots[i].target, sizeof(kept.shots[i].target));
        kept.shots[i].made_at = ring->shots[i].made_at;
        kept.shots[i].used = ring->shots[i].used != 0;
    }
    kept.count = ring->count;
    kept.pending = ring->pending != 0;
    float flash[3], target[3];
    int found = aae_freeaim_match(&kept, origin, start, muzzle, now, flash, target);
    if (found) {
        memcpy(origin, flash, sizeof(flash));
        memcpy(start, target, sizeof(target));
        memcpy(muzzle, flash, sizeof(flash));
    }
    for (int i = 0; i < AAE_FREEAIM_SHOTS; i++)
        ring->shots[i].used = kept.shots[i].used;
    ring->pending = kept.pending;
    return found;
}

// ---- the client's copy of a shot

static const endpos_shot_case *side_case;
static endpos_shot_result *side_result;
static uint8_t *side_cgs; // the local clients' cg_t, and what comes after them
static uint8_t side_cent[0x400], side_frame_room[0x100], side_objects[2];
static const uint8_t side_caller = 0;
static uint32_t side_tag;
// the dvars: a kind at +0x1c (8: one with text), the text
static struct {
    uint8_t game_side[0x40];
    const char *name;
    bool there;
    char text[64];
} side_dvars[] = { { .name = "mapname", .there = true, .text = "zm_test" },
                   { .name = "aae_freeaim_target" },
                   { .name = "aae_freeaim_target_1" } };
static void *side_find(uint32_t hash) {
    for (size_t i = 0; i < sizeof(side_dvars) / sizeof(side_dvars[0]); i++)
        if (side_dvars[i].there && gsc_hash(side_dvars[i].name) == hash)
            return &side_dvars[i];
    return NULL;
}
static const char *side_string(void *dvar) {
    return ((typeof(side_dvars[0]) *)dvar)->text;
}
static uint64_t side_clock(void) {
    return side_case->now;
}
static void *side_dobj(int number, int local_client) {
    (void)number;
    return side_case->has_model[local_client] ? &side_objects[local_client] : NULL;
}
static int side_tag_position(const uint8_t *cent, void *object, uint32_t tag, float position[3]) {
    (void)cent, (void)tag;
    int which = object == &side_objects[1];
    if (!side_case->has_tag[which])
        return 0;
    memcpy(position, side_case->flash[which], 3 * sizeof(float));
    return 1;
}
static void side_endpos(uint64_t seed, float spread, float spread2, float *start, uint64_t end, uint64_t direction,
                        float fraction, float fraction2, float *forward, float *right, float range, float *up,
                        uint64_t weapon, uint64_t pellet, uint64_t pellets) {
    (void)seed, (void)spread, (void)spread2, (void)start, (void)end, (void)direction, (void)fraction, (void)fraction2;
    (void)range, (void)weapon, (void)pellet, (void)pellets;
    side_result->calls++;
    memcpy(side_result->directions, forward, 3 * sizeof(float));
    memcpy(side_result->directions + 3, right, 3 * sizeof(float));
    memcpy(side_result->directions + 6, up, 3 * sizeof(float));
}

static void side_client_shot(const endpos_shot_case *c, endpos_shot_result *r) {
    // what the one who includes this had, put back afterwards
    void *(*find)(uint32_t) = Dvar_FindByHash;
    const char *(*text)(void *) = Dvar_GetString;
    uint8_t *const *cgs = cg_clients;
    uint64_t (*clock)(void) = now_ms;
    void *(*dobj)(int, int) = Com_GetClientDObj;
    int (*tag_position)(const uint8_t *, void *, uint32_t, float *) = CG_DObjGetWorldTagPos;
    const uint32_t *tag = tag_flash_name;
    void (*endpos)(uint64_t, float, float, float *, uint64_t, uint64_t, float, float, float *, float *, float, float *,
                   uint64_t, uint64_t, uint64_t) = orig_Bullet_Endpos;
    const void *asked_from = endpos_asked_from[1];
    aae_freeaim_client_t had_clients[Local_Clients];
    aae_freeaim_tracers_t had_tracers[Local_Clients];
    memcpy(had_clients, clients, sizeof(clients));
    memcpy(had_tracers, tracers, sizeof(tracers));

    if (side_cgs == NULL)
        side_cgs = calloc(3, Cg_Size);
    side_case = c, side_result = r;
    Dvar_FindByHash = side_find, Dvar_GetString = side_string, cg_clients = &side_cgs, now_ms = side_clock;
    Com_GetClientDObj = side_dobj, CG_DObjGetWorldTagPos = side_tag_position, tag_flash_name = &side_tag;
    orig_Bullet_Endpos = side_endpos;
    endpos_asked_from[1] = &side_caller;
    for (int k = 0; k < Local_Clients; k++) {
        uint8_t *cg = side_cgs + (size_t)k * Cg_Size;
        memcpy(cg, &c->cg_number[k], sizeof(c->cg_number[k]));
        cg[Cg_ThirdPerson] = 1;
        // free aim on: seen now, in third person for long; off: not seen for a second
        clients[k].cg = c->seen[k] ? cg : NULL;
        clients[k].seen_at = c->on[k] ? c->now : c->now - 1000;
        clients[k].third_person = (c->now - 1000) << 2 | 2;
        typeof(side_dvars[0]) *dvar = &side_dvars[1 + k];
        dvar->there = c->has_target[k] != 0;
        int kind = Dvar_Type_String;
        memcpy(dvar->game_side + Dvar_Type, &kind, sizeof(kind));
        snprintf(dvar->text, sizeof(dvar->text), "%d %d %d", c->target[k][0], c->target[k][1], c->target[k][2]);
        memset(&tracers[k], 0, sizeof(tracers[k]));
        tracers[k].count = c->count[k];
        memcpy(tracers[k].flash, c->cache[k], sizeof(tracers[k].flash));
        tracers[k].flash_at = c->cache_at[k];
    }
    int kind = Dvar_Type_String;
    memcpy(side_dvars[0].game_side + Dvar_Type, &kind, sizeof(kind));
    side_tag = c->tag_ready ? 0x2a5 : 0;
    uint8_t *frame = side_frame_room + 0x80;
    const uint8_t *cent = c->frame == 1 ? side_cent : NULL;
    memcpy(frame + Fire_Frame_Cent, &cent, sizeof(cent));
    memcpy(side_cent + Cent_Number, &c->number, sizeof(c->number));
    float start[3], directions[9];
    memcpy(start, c->start, sizeof(start));
    memcpy(directions, c->directions, sizeof(directions));
    client_shot(&side_caller, c->frame == 0 ? NULL : frame, 0, 0.0f, 0.0f, start, 0, 0, 0.0f, 0.0f, directions,
                directions + 3, 8192.0f, directions + 6, 0, 0, 1);
    for (int k = 0; k < Local_Clients; k++) {
        r->count[k] = tracers[k].count;
        r->pending[k] = tracers[k].pending;
        const aae_freeaim_shot_t *shot = &tracers[k].shots[(tracers[k].count - 1) % AAE_FREEAIM_SHOTS];
        memcpy(r->kept[k].start, shot->start, sizeof(shot->start));
        memcpy(r->kept[k].flash, shot->flash, sizeof(shot->flash));
        memcpy(r->kept[k].target, shot->target, sizeof(shot->target));
        r->kept[k].made_at = shot->made_at;
        memcpy(r->cache[k], tracers[k].flash, sizeof(tracers[k].flash));
        r->cache_at[k] = tracers[k].flash_at;
    }

    Dvar_FindByHash = find, Dvar_GetString = text, cg_clients = cgs, now_ms = clock;
    Com_GetClientDObj = dobj, CG_DObjGetWorldTagPos = tag_position, tag_flash_name = tag;
    orig_Bullet_Endpos = endpos;
    endpos_asked_from[1] = asked_from;
    memcpy(clients, had_clients, sizeof(clients));
    memcpy(tracers, had_tracers, sizeof(tracers));
}
