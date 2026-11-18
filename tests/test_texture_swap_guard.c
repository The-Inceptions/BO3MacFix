// texture_swap_guard.c: which callers of the game's untrack function hand their view to the game's delayed release,
// and what that does to a view's life against a made-up view, untrack, delayed release and render thread (the test
// includes the source itself).
#include <stdio.h>
#include <string.h>
#include "../source/texture_swap_guard.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Base 0x100000000ULL
#define Delay_Frames 4 // the game's dvar 0x275fc2f8 at its default

// --- the made-up game --------------------------------------------------------------------------------------------
static void *hooked_at, *hooked_with;
static int hooks;
static int game_untrack(void *view, uint64_t category);
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    hooked_at = address, hooked_with = fake_func, hooks++;
    *out_origin_func = (void *)game_untrack;
    return 0;
}

// a view as ASL has it for this purpose: a vtable whose second entry is AddRef and third Release, a reference count,
// and what the texture memory table holds for it. Freeing it is only counted; a reference taken while the count is 0
// is what the render thread does to a view another thread is destroying (the fault this guard is for)
typedef struct {
    void *const *vtable;
    int refs;
    int tracked;   // the view's count in the game's texture memory table
    int destroyed; // times its last reference went
    int revived;   // references taken while it had none
} view_t;
static void view_query(void *view) {
    (void)view;
}
static void view_add_ref(void *opaque) {
    view_t *view = opaque;
    if (view->refs == 0)
        view->revived++;
    view->refs++;
}
static void view_release(void *opaque) {
    view_t *view = opaque;
    if (--view->refs == 0)
        view->destroyed++;
}
static void *const view_vtable[] = { (void *)view_query, (void *)view_add_ref, (void *)view_release };
static view_t new_view(void) {
    return (view_t){ view_vtable, 1, 1, 0, 0 };
}

// the untrack function: counts the view's entry down, returns what is left
static int untracks;
static uint64_t untrack_category;
static void *untracked;
static int game_untrack(void *opaque, uint64_t category) {
    untracks++, untracked = opaque, untrack_category = category;
    if (opaque == NULL)
        return 0;
    view_t *view = opaque;
    return view->tracked > 0 ? --view->tracked : 0;
}

// the delayed release: a queue with frame stamps; each frame releases (untracking first when asked) what is older
// than Delay_Frames, as 0x10129b5af does
#define Queue_Size 64
static struct {
    uint32_t stamp;
    bool untrack;
    void *view;
} queue[Queue_Size];
static int queued, released_from_queue;
static uint32_t frame_stamp;
static uint64_t last_untrack_flag;
static void game_delayed_release(void *view, uint64_t untrack) {
    last_untrack_flag = untrack;
    queue[queued].stamp = frame_stamp, queue[queued].untrack = (uint8_t)untrack != 0, queue[queued].view = view;
    queued++;
}
static void game_frame(void) {
    frame_stamp++;
    for (int i = 0; i < queued; i++) {
        if (queue[i].view == NULL || queue[i].stamp + Delay_Frames >= frame_stamp)
            continue;
        if (queue[i].untrack)
            game_untrack(queue[i].view, 0);
        view_release(queue[i].view);
        queue[i].view = NULL;
        released_from_queue++;
    }
}

// the game's ring as the guard reads it: its head and tail (the made-up queue above stands for its rules; these say how
// full it is)
#define Ring_At 0x1020d9a80ULL
static uint64_t game_ring_head, game_ring_tail;
static void set_ring_used(uint32_t used) {
    game_ring_head = used == 0 ? 0 : Ring_At + 100 * Ring_Entry_Size;
    game_ring_tail = used == 0 ? Ring_At : Ring_At + ((100 + used) % Ring_Entries) * Ring_Entry_Size;
}

// the frame of the game function a place is in: [1] where that function returns to
static void *frame_from_stub_maker[2] = { NULL, (void *)(Base + ADDR_TextureSwap_StubMakerRet) };
static void *frame_from_default_reset[2] = { NULL, (void *)(Base + ADDR_TextureSwap_DefaultResetRet) };

// a place's counter, looked up by its return offset; a place that isn't in the table reads as 0xffffffff
static uint32_t at(const uint32_t *counts, uint32_t place) {
    const texture_swap_site_t *site = texture_swap_guard_site(place);
    return site != NULL ? counts[site - sites] : 0xffffffffu;
}

static void reset_game(void) {
    untracks = 0, untracked = NULL, untrack_category = 99;
    memset(queue, 0, sizeof(queue));
    queued = released_from_queue = 0;
    last_untrack_flag = 0;
    memset(handed_over, 0, sizeof(handed_over));
    memset(passed_full, 0, sizeof(passed_full));
    memset(passed_unguarded, 0, sizeof(passed_unguarded));
    set_ring_used(0);
}

static void set_up(void) {
    hooks = 0;
    texture_swap_guard_init(Base, 1);
    // the hook is set; the game functions it calls are this test's
    R_UntrackTextureMemory = game_untrack;
    R_DelayedReleaseTexture = game_delayed_release;
    ring_head = &game_ring_head, ring_tail = &game_ring_tail;
    reset_game();
}

// what the game does at a place: untrack the holder's view (through the hook, as called from that place), then
// release it; the release-before-store places store the new view only after that
static void game_untrack_and_release_from(view_t *old, uint32_t return_offset, void *const *frame) {
    texture_swap_guard_untrack(old, 0, Base + return_offset, frame);
    view_release(old);
}
static void game_untrack_and_release(view_t *old, uint32_t return_offset) {
    game_untrack_and_release_from(old, return_offset, frame_from_default_reset);
}

// --- the tests -----------------------------------------------------------------------------------------------------
static void test_sites(void) {
    const uint32_t places[] = { 0xe4a5a1, 0xe4a68f, 0x138f173, 0x138f20d, 0x138f25b, 0x129c15a, 0x129c19c };
    CHECK(Site_Count == sizeof(places) / sizeof(places[0]));
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        const texture_swap_site_t *site = texture_swap_guard_site(places[i]);
        CHECK(site != NULL && site->return_offset == places[i] && site->function != NULL);
    }
    // the other callers keep the game's untrack: the delayed release's own (queue full 0x129b4dd, processing
    // 0x129b5af's), R_Shutdown's render-target sets (0xd0778b + 5), the streamer's mip swap's neighbour (0x138ecb7
    // queues itself), the instruction before or after a place, the image release (the unloads sync the render thread)
    CHECK(texture_swap_guard_site(0x129c15a)->unguarded_caller == 0x13497db &&
          texture_swap_guard_site(0x129c19c)->unguarded_caller == 0x13497db);
    for (size_t i = 0; i < 5; i++)
        CHECK(texture_swap_guard_site(places[i])->unguarded_caller == 0);
    const uint32_t others[] = { 0x129b4dd, 0x129b62a, 0xd07790, 0xd79496 + 5, 0xe4a5a0, 0xe4a5a2, 0x138f172, 0,
                                0x129b2b3, 0x129b2e8 }; // the image release: not a place any more
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++)
        CHECK(texture_swap_guard_site(others[i]) == NULL);
}

static void test_rule(void) {
    const texture_swap_site_t *site = texture_swap_guard_site(ADDR_TextureSwap_PreviewRet);
    view_t view = new_view();
    CHECK(texture_swap_guard_delays(site, &view, 0, 0, 0));
    CHECK(!texture_swap_guard_delays(NULL, &view, 0, 0, 0));
    CHECK(!texture_swap_guard_delays(site, NULL, 0, 0, 0));
    CHECK(!texture_swap_guard_delays(site, &view, 1, 0, 0));
    // only esi counts: what is above it in rsi is whatever the caller had there
    CHECK(texture_swap_guard_delays(site, &view, 0xdead00000000ULL, 0, 0));
    CHECK(!texture_swap_guard_delays(site, &view, 0xdead00000001ULL, 0, 0));
    // a place without an unguarded caller doesn't look at who called its function
    CHECK(texture_swap_guard_delays(site, &view, 0, ADDR_TextureSwap_StubMakerRet, 0));
    // the stub assignment: not from the stub maker, guarded from the default reset (and anybody else)
    const texture_swap_site_t *stub = texture_swap_guard_site(ADDR_TextureSwap_ImageAssignRet);
    const texture_swap_site_t *stub2 = texture_swap_guard_site(ADDR_TextureSwap_ImageAssignRet2);
    CHECK(!texture_swap_guard_delays(stub, &view, 0, ADDR_TextureSwap_StubMakerRet, 0));
    CHECK(!texture_swap_guard_delays(stub2, &view, 0, ADDR_TextureSwap_StubMakerRet, 0));
    CHECK(texture_swap_guard_delays(stub, &view, 0, ADDR_TextureSwap_DefaultResetRet, 0));
    CHECK(texture_swap_guard_delays(stub2, &view, 0, 0x123456, 0));
    // the room kept in the game's ring
    CHECK(texture_swap_guard_delays(site, &view, 0, 0, Ring_Entries - Ring_Room_Kept - 1));
    CHECK(!texture_swap_guard_delays(site, &view, 0, 0, Ring_Entries - Ring_Room_Kept));
    CHECK(!texture_swap_guard_delays(site, &view, 0, 0, Ring_Entries));
    CHECK(Ring_Entries - Ring_Room_Kept == 7158);
}

// the ring's fill from its head and tail, as 0x10129b485 keeps them (head 0: empty; head == tail: full)
static void test_ring_used(void) {
    const uint64_t r = Ring_At, end = Ring_At + (uint64_t)Ring_Entries * Ring_Entry_Size;
    CHECK(texture_swap_guard_ring_used(0, r) == 0 && texture_swap_guard_ring_used(0, r + 160) == 0);
    CHECK(texture_swap_guard_ring_used(r, r) == Ring_Entries && texture_swap_guard_ring_used(r + 320, r + 320) == Ring_Entries);
    CHECK(texture_swap_guard_ring_used(r, r + 5 * 16) == 5);
    CHECK(texture_swap_guard_ring_used(r + 16, r) == Ring_Entries - 1);         // one behind the head: wrapped
    CHECK(texture_swap_guard_ring_used(r + 8000 * 16, r + 10 * 16) == 192);   // 182 to the end, 10 from the start
    CHECK(texture_swap_guard_ring_used(r, end - 16) == Ring_Entries - 1);
    CHECK(texture_swap_guard_ring_used(r + 16, end) == Ring_Entries - 1);       // a tail at the end is the start
    for (uint32_t used = 0; used < Ring_Entries; used += 97) {
        set_ring_used(used);
        CHECK(texture_swap_guard_ring_used(game_ring_head, game_ring_tail) == used);
    }
}

static void test_logs(void) {
    CHECK(!texture_swap_guard_logs(0));
    CHECK(texture_swap_guard_logs(1) && texture_swap_guard_logs(2) && texture_swap_guard_logs(3));
    CHECK(texture_swap_guard_logs(4) && texture_swap_guard_logs(8) && texture_swap_guard_logs(1u << 31));
    CHECK(!texture_swap_guard_logs(5) && !texture_swap_guard_logs(6) && !texture_swap_guard_logs(7));
    CHECK(!texture_swap_guard_logs(9) && !texture_swap_guard_logs(12));
}

// the workshop preview's swap: release before store. Without the guard the old view dies at the release while the
// image still points at it, and a render thread that reads it then brings it back from 0; with the guard it lives on
// until the delayed release, and dies once, untracked
static void test_preview_swap(void) {
    // without the guard (any other place: the game's own untrack)
    reset_game();
    view_t old = new_view(), fresh = new_view();
    view_t *image_view = &old;
    game_untrack_and_release(image_view, 0x123456);
    CHECK(untracks == 1 && old.tracked == 0 && old.destroyed == 1 && queued == 0);
    view_add_ref(image_view); // the render thread reads the image's field before the store
    CHECK(old.revived == 1);
    image_view = &fresh;

    // with it
    reset_game();
    old = new_view(), fresh = new_view();
    image_view = &old;
    game_untrack_and_release(image_view, ADDR_TextureSwap_PreviewRet);
    CHECK(untracks == 0 && old.tracked == 1 && old.refs == 1 && old.destroyed == 0);
    CHECK(queued == 1 && queue[0].view == &old && queue[0].untrack && last_untrack_flag == 1);
    view_add_ref(image_view); // the render thread reads the field before the store: a live view
    image_view = &fresh;
    CHECK(old.revived == 0 && old.refs == 2);
    for (int frame = 0; frame < Delay_Frames; frame++)
        game_frame();
    view_release(&old); // the slot is bound to something else
    CHECK(old.destroyed == 0 && old.refs == 1 && old.tracked == 1);
    game_frame();
    CHECK(old.destroyed == 1 && old.refs == 0 && old.tracked == 0 && old.revived == 0 && released_from_queue == 1);
    CHECK(untracks == 1 && untracked == &old && untrack_category == 0);
    CHECK(at(handed_over, ADDR_TextureSwap_PreviewRet) == 1 && fresh.destroyed == 0);
    (void)image_view;
}

// the loading picture's swap and the stubs' assignment: the same pattern at their places
static void test_release_before_store_places(void) {
    const uint32_t places[] = { ADDR_TextureSwap_PreviewLoadingRet, ADDR_TextureSwap_ImageAssignRet,
                                ADDR_TextureSwap_ImageAssignRet2 };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        reset_game();
        view_t old = new_view();
        game_untrack_and_release(&old, places[i]);
        view_add_ref(&old);
        view_release(&old);
        CHECK(old.destroyed == 0 && old.revived == 0 && old.tracked == 1);
        for (int frame = 0; frame <= Delay_Frames; frame++)
            game_frame();
        CHECK(old.destroyed == 1 && old.tracked == 0 && old.refs == 0);
    }
}

// the streamer's swap and the image release: store (or clear) first, release after. The render thread read the field
// just before and takes its reference after the release: without the guard a dead view, with it a live one
static void test_store_before_release_places(void) {
    const uint32_t places[] = { ADDR_TextureSwap_StreamerRet, ADDR_TextureSwap_StreamerRet2,
                                ADDR_TextureSwap_StreamerRet3 };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        reset_game();
        view_t old = new_view();
        view_t *read_by_render_thread = &old; // read before the swap stored the new view
        game_untrack_and_release(&old, places[i]);
        view_add_ref(read_by_render_thread);
        CHECK(old.revived == 0 && old.refs == 2);
        view_release(read_by_render_thread);
        for (int frame = 0; frame <= Delay_Frames; frame++)
            game_frame();
        CHECK(old.destroyed == 1 && old.tracked == 0 && old.refs == 0 && old.revived == 0);
        CHECK(at(handed_over, places[i]) == 1);
    }
}

// a view that a slot still holds when the delayed release comes: the slot's release is then the last one
static void test_view_still_bound(void) {
    reset_game();
    view_t old = new_view();
    view_add_ref(&old); // a context slot holds it
    game_untrack_and_release(&old, ADDR_TextureSwap_StreamerRet);
    for (int frame = 0; frame <= Delay_Frames + 3; frame++)
        game_frame();
    CHECK(old.destroyed == 0 && old.refs == 1 && old.tracked == 0);
    view_release(&old);
    CHECK(old.destroyed == 1 && old.revived == 0);
}

// the game's untrack for everything else, with what it returns
static void test_passes_through(void) {
    reset_game();
    view_t view = new_view();
    view.tracked = 2;
    CHECK(texture_swap_guard_untrack(&view, 0, Base + 0x123456, NULL) == 1);
    CHECK(untrack_category == 0);
    view.tracked = 2;
    CHECK(texture_swap_guard_untrack(&view, 1, Base + 0x123456, NULL) == 1 && untrack_category == 1 && untracks == 2);
    untracks = 1;
    CHECK(untracks == 1 && untracked == &view && view.tracked == 1 && queued == 0 && view.refs == 1);
    // a place's call in another category
    CHECK(texture_swap_guard_untrack(&view, 1, Base + ADDR_TextureSwap_PreviewRet, NULL) == 0);
    CHECK(untracks == 2 && untrack_category == 1 && queued == 0 && view.refs == 1);
    // no view
    CHECK(texture_swap_guard_untrack(NULL, 0, Base + ADDR_TextureSwap_PreviewRet, NULL) == 0);
    CHECK(untracks == 3 && untracked == NULL && queued == 0);
    // the right offset from another base is another place
    CHECK(texture_swap_guard_untrack(&view, 0, 0x200000000ULL + ADDR_TextureSwap_PreviewRet, NULL) == 0);
    CHECK(untracks == 4 && queued == 0);
    // and the hook itself, called from this test: not a place of the game
    view.tracked = 1;
    CHECK(fake_R_UntrackTextureMemory(&view, 0) == 0 && untracks == 5 && view.tracked == 0 && queued == 0);
}

// the stub assignment called by a zone unload's stub maker: the game's own untrack and release (nothing draws the
// menus then); called by the default reset: handed over
static void test_stub_maker(void) {
    reset_game();
    view_t from_unload = new_view(), from_reset = new_view();
    game_untrack_and_release_from(&from_unload, ADDR_TextureSwap_ImageAssignRet, frame_from_stub_maker);
    CHECK(from_unload.destroyed == 1 && from_unload.tracked == 0 && untracks == 1 && queued == 0);
    CHECK(at(passed_unguarded, ADDR_TextureSwap_ImageAssignRet) == 1 && at(handed_over, ADDR_TextureSwap_ImageAssignRet) == 0 &&
          at(passed_full, ADDR_TextureSwap_ImageAssignRet) == 0);
    game_untrack_and_release_from(&from_reset, ADDR_TextureSwap_ImageAssignRet2, frame_from_default_reset);
    CHECK(from_reset.destroyed == 0 && from_reset.refs == 1 && queued == 1 && at(handed_over, ADDR_TextureSwap_ImageAssignRet2) == 1);
    for (int frame = 0; frame <= Delay_Frames; frame++)
        game_frame();
    CHECK(from_reset.destroyed == 1 && from_reset.tracked == 0 && from_reset.revived == 0);
    // and through the hook, as the game enters it: its frame is this test's, not a place's: the game's untrack
    view_t other = new_view();
    CHECK(fake_R_UntrackTextureMemory(&other, 0) == 0 && other.tracked == 0 && queued == 1);
}

// a nearly full ring: the view goes the game's own way (untracked and released at once), and is handed over again
// once the ring has room
static void test_full_ring(void) {
    reset_game();
    set_ring_used(Ring_Entries - Ring_Room_Kept);
    view_t passed = new_view();
    game_untrack_and_release(&passed, ADDR_TextureSwap_PreviewRet);
    CHECK(passed.destroyed == 1 && passed.tracked == 0 && untracks == 1 && queued == 0 &&
          at(passed_full, ADDR_TextureSwap_PreviewRet) == 1);
    set_ring_used(Ring_Entries);
    view_t full = new_view();
    game_untrack_and_release(&full, ADDR_TextureSwap_StreamerRet);
    CHECK(full.destroyed == 1 && queued == 0 && at(passed_full, ADDR_TextureSwap_StreamerRet) == 1);
    set_ring_used(Ring_Entries - Ring_Room_Kept - 1);
    view_t room = new_view();
    game_untrack_and_release(&room, ADDR_TextureSwap_PreviewRet);
    CHECK(room.destroyed == 0 && queued == 1 && at(handed_over, ADDR_TextureSwap_PreviewRet) == 1 &&
          at(passed_full, ADDR_TextureSwap_PreviewRet) == 1);
    // the stub maker's call is not a full ring's
    set_ring_used(Ring_Entries);
    view_t stub = new_view();
    game_untrack_and_release_from(&stub, ADDR_TextureSwap_ImageAssignRet, frame_from_stub_maker);
    CHECK(stub.destroyed == 1 && at(passed_unguarded, ADDR_TextureSwap_ImageAssignRet) == 1 &&
          at(passed_full, ADDR_TextureSwap_ImageAssignRet) == 0);
}

// many swaps in a row: each view lives exactly its frames and dies once
static void test_many(void) {
    reset_game();
    enum { Views = 40 };
    view_t views[Views];
    for (int i = 0; i < Views; i++) {
        views[i] = new_view();
        game_untrack_and_release(&views[i], i % 2 == 0 ? ADDR_TextureSwap_PreviewRet : ADDR_TextureSwap_StreamerRet);
        game_frame();
        for (int j = 0; j <= i; j++) {
            bool old_enough = i - j >= Delay_Frames;
            CHECK(views[j].destroyed == (old_enough ? 1 : 0));
        }
    }
    for (int frame = 0; frame <= Delay_Frames; frame++)
        game_frame();
    for (int i = 0; i < Views; i++)
        CHECK(views[i].destroyed == 1 && views[i].tracked == 0 && views[i].refs == 0 && views[i].revived == 0);
    CHECK(at(handed_over, ADDR_TextureSwap_PreviewRet) == Views / 2 &&
          at(handed_over, ADDR_TextureSwap_StreamerRet) == Views / 2 && released_from_queue == Views);
}

static void test_init(void) {
    hooks = 0, hooked_at = hooked_with = NULL;
    texture_swap_guard_init(Base, 0);
    CHECK(hooks == 0);
    texture_swap_guard_init(Base, 1);
    CHECK(hooks == 1 && hooked_at == (void *)(Base + ADDR_R_UntrackTextureMemory) &&
          hooked_with == (void *)fake_R_UntrackTextureMemory);
    CHECK(R_UntrackTextureMemory == game_untrack && orig_R_UntrackTextureMemory == game_untrack);
    CHECK(R_DelayedReleaseTexture == (void *)(Base + ADDR_R_DelayedReleaseTexture) && base_address == Base);
    CHECK(ring_head == (uint64_t *)(Base + ADDR_DelayedRelease_Head) &&
          ring_tail == (uint64_t *)(Base + ADDR_DelayedRelease_Tail));
    CHECK(ADDR_DelayedRelease_Head == 0x20d9a70 && ADDR_DelayedRelease_Tail == 0x20d9a78 &&
          ADDR_DelayedRelease_Ring == 0x20d9a80 && ADDR_TextureSwap_StubMakerRet == 0x13497db &&
          ADDR_TextureSwap_DefaultResetRet == 0x129c23e);
    // the addresses this rests on, as the game's code has them (wip/night/crash-render.md)
    CHECK(ADDR_R_UntrackTextureMemory == 0x126dde1 && ADDR_R_DelayedReleaseTexture == 0x129b485);
}

int main(void) {
    test_init();
    set_up();
    test_sites();
    test_rule();
    test_ring_used();
    test_logs();
    test_preview_swap();
    test_release_before_store_places();
    test_store_before_release_places();
    test_view_still_bound();
    test_passes_through();
    test_stub_maker();
    test_full_ring();
    test_many();
    if (failures == 0)
        printf("test_texture_swap_guard: all passed\n");
    return failures == 0 ? 0 : 1;
}
