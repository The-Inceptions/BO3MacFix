// TextureSwapGuard: a texture view the game replaces while the render thread may still be reading it goes to the
// game's own delayed release instead of dying at once.
//
// Aspyr's Direct3D 11 layer on Metal (ASL::DX11Metal) counts references on its objects. The render thread binds a
// picture by handing the context the address of the image's view field; the context reads the view from that field
// and only then, a few instructions and possibly the destruction of the slot's previous view later, takes its own
// reference (PSSetShaderResources 0x10002b7ae: the read at 0x10002b819, the AddRef at 0x10002b87c). Nothing ties
// the two together for the other threads. Some places of the game drop an image's view right away on the main thread
// while the menus are drawn: the workshop preview swap (Lua's Engine.UpdateModPreviewImage, the CHANGE MAP list)
// releases it while the image still points at it and stores the new one only after; the streamer's view swap (the
// menus' streamed pictures, the globe's twenty among them) stores first and releases a moment later. When that
// release is the last one, the view's destructor waits for the device's mutex, which any thread creating a texture
// holds for the whole upload, while the dead view is still intact. If the render thread reads the view in that time,
// its AddRef brings it back from 0 to 1, it binds and unbinds it, and its own release starts a second destruction: one
// of the two then frees the block a second time. That is one of the two ways to get the report of the user's crash of
// 2026-11-13 on the render thread (AtomicRelease closes the other; wip/night/crash-render.md has the whole of it).
//
// The game has a cure itself and uses it in one place: its streamer's mip swap (0x10138ecb7) hands the old view to a
// delayed release (0x10129b485), a ring of 8182 entries that untracks and releases each entry once 4 of its ticks
// have passed (the dvar 0x275fc2f8). It ticks once per frame of the render thread, at the frame's start, before the
// thread draws anything; a load that pauses the streamer ticks it six times at once; and a full ring releases its
// oldest entry at once, whatever its age. This guard makes the places above use it: our hook on the untrack function,
// which each of them calls right before its release, recognises them by their return address, takes a reference of
// its own and queues the view for that delayed release instead of untracking it now; the caller's own release then
// drops only the holder's reference. The view and its texture live on until the ring lets them go, untracked then. So
// a view that one of these places drops is not freed while the render thread can still be in the middle of taking it:
// as a rule it lives at least 4 of the render thread's frames longer, less when a load pauses the streamer, and never
// less long than without the guard.
//
// Where nothing can be reading the field the guard stays out of the way, and it never fills the ring:
// - the stub assignment 0x10129c113 (an image takes the default picture's views and releases its own) runs for
//   every frontend picture when a zone is unloaded into default stubs (twice in every mod load from the stock MODS
//   menu, 12,000 views). That unload syncs the render thread first, and while it runs the render thread only draws
//   the loading icon (its remote-update frames skip the client frame that draws the menus), so the stub maker's call
//   (return 0x1013497db) gets the game's untrack. Its other caller (0x10129c1e8, an image reset to the default
//   picture, from another thread's loop) stays guarded.
// - the image release (0x10129b297) is not a place: both unloads that call it sync the render thread first and give
//   the image struct back to its pool right after (the review's finding).
// - when the ring holds Ring_Entries - Ring_Room_Kept entries or more, the view goes to the game's own untrack and
//   release at once, so the game's own users of the ring always find room.
// Every other caller of the untrack function, and every one with the guard off, gets the game's untrack.
//
// It applies with or without a mod: the preview swap is All-around Enhancement's CHANGE MAP list, the streamer is any
// menu with streamed pictures.
#include <stdio.h>
#include <stdlib.h>
#include "dobby.h"
#include "offsets.h"
#include "texture_swap_guard.h"

#define Delayed_Category 0 // the texture memory category of every place below, and the one the delayed release untracks

static const texture_swap_site_t sites[] = {
    { ADDR_TextureSwap_PreviewLoadingRet, 0, "the workshop loading picture's swap" },
    { ADDR_TextureSwap_PreviewRet, 0, "the workshop preview picture's swap" },
    { ADDR_TextureSwap_StreamerRet, 0, "the streamer's view swap" },
    { ADDR_TextureSwap_StreamerRet2, 0, "the streamer's view swap (2)" },
    { ADDR_TextureSwap_StreamerRet3, 0, "the streamer's view swap (3)" },
    { ADDR_TextureSwap_ImageAssignRet, ADDR_TextureSwap_StubMakerRet, "an image reset to the default picture" },
    { ADDR_TextureSwap_ImageAssignRet2, ADDR_TextureSwap_StubMakerRet,
      "an image reset to the default picture (second view)" },
};
#define Site_Count (sizeof(sites) / sizeof(sites[0]))

static uint64_t base_address;
static bool debug;
static uint32_t handed_over[Site_Count], passed_full[Site_Count], passed_unguarded[Site_Count];
static int (*R_UntrackTextureMemory)(void *view, uint64_t category);
static void (*R_DelayedReleaseTexture)(void *view, uint64_t untrack);
static uint64_t *ring_head, *ring_tail; // the game's, read without its lock: a count to compare with a limit

const texture_swap_site_t *texture_swap_guard_site(uint64_t return_offset) {
    for (size_t i = 0; i < Site_Count; i++)
        if (sites[i].return_offset == return_offset)
            return &sites[i];
    return NULL;
}

uint32_t texture_swap_guard_ring_used(uint64_t head, uint64_t tail) {
    if (head == 0)
        return 0;
    if (head == tail)
        return Ring_Entries;
    int64_t entries = ((int64_t)tail - (int64_t)head) / Ring_Entry_Size;
    return (uint32_t)(((entries % Ring_Entries) + Ring_Entries) % Ring_Entries);
}

bool texture_swap_guard_delays(const texture_swap_site_t *site, const void *view, uint64_t category,
                               uint64_t site_caller_offset, uint32_t ring_used) {
    // the game passes the category in esi; only the low half counts, as in the untrack function itself
    if (site == NULL || view == NULL || (uint32_t)category != Delayed_Category)
        return false;
    if (site->unguarded_caller != 0 && site_caller_offset == site->unguarded_caller)
        return false;
    return ring_used < Ring_Entries - Ring_Room_Kept;
}

bool texture_swap_guard_logs(uint32_t times) {
    return times != 0 && (times <= 3 || (times & (times - 1)) == 0);
}

int texture_swap_guard_untrack(void *view, uint64_t category, uint64_t return_address, void *const *site_frame) {
    const texture_swap_site_t *site = texture_swap_guard_site(return_address - base_address);
    if (site == NULL)
        return R_UntrackTextureMemory(view, category);
    // where the place's function returns to: only asked for the places that have an unguarded caller (the stub
    // assignment, whose function keeps an rbp frame like every function with a place)
    uint64_t site_caller = site->unguarded_caller != 0 ? (uint64_t)site_frame[1] - base_address : 0;
    uint32_t used = texture_swap_guard_ring_used(__atomic_load_n(ring_head, __ATOMIC_RELAXED),
                                                 __atomic_load_n(ring_tail, __ATOMIC_RELAXED));
    if (!texture_swap_guard_delays(site, view, category, site_caller, used)) {
        if (site->unguarded_caller != 0 && site_caller == site->unguarded_caller) {
            uint32_t times = __atomic_add_fetch(&passed_unguarded[site - sites], 1, __ATOMIC_RELAXED);
            if (debug && texture_swap_guard_logs(times))
                printf("textureswap: %s, from a zone unload's stub maker: the game's own untrack (%u times there)\n",
                       site->function, times);
        } else if (used >= Ring_Entries - Ring_Room_Kept && view != NULL) {
            uint32_t times = __atomic_add_fetch(&passed_full[site - sites], 1, __ATOMIC_RELAXED);
            if (debug && texture_swap_guard_logs(times))
                printf("textureswap: the game's delayed release holds %u, so %s releases at once (%u times there)\n",
                       used, site->function, times);
        }
        return R_UntrackTextureMemory(view, category);
    }
    // the caller releases the holder's reference right after this call: our own reference keeps the view alive through
    // it, and the delayed release gives ours back, untracking the view first, when its ticks have passed. AddRef is
    // the second entry of every ASL interface's vtable
    void (*add_ref)(void *) = (*(void (***)(void *))view)[1];
    add_ref(view);
    R_DelayedReleaseTexture(view, 1);
    uint32_t times = __atomic_add_fetch(&handed_over[site - sites], 1, __ATOMIC_RELAXED);
    if (debug && texture_swap_guard_logs(times))
        printf("textureswap: %s handed view %p to the game's delayed release (%u times there, %u queued)\n",
               site->function, view, times, used + 1);
    // what the untrack function returns (the entry's count left) isn't read at any of these places
    return 0;
}

// R_UntrackTextureMemory(view, category). Entered by a jump from the pre-patched executable's stub (or Dobby's), so
// the return address is the game's, and this function's saved rbp is the frame of the game function that called it
install_hook_name(R_UntrackTextureMemory, int, void *view, uint64_t category) {
    void *const *frame = __builtin_frame_address(0);
    return texture_swap_guard_untrack(view, category, (uint64_t)__builtin_return_address(0), (void *const *)frame[0]);
}

void texture_swap_guard_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_TEXTURESWAPDEBUG");
    debug = env != NULL && atoi(env) != 0;
    R_DelayedReleaseTexture = (void *)(base_address + ADDR_R_DelayedReleaseTexture);
    ring_head = (uint64_t *)(base_address + ADDR_DelayedRelease_Head);
    ring_tail = (uint64_t *)(base_address + ADDR_DelayedRelease_Tail);
    install_hook_R_UntrackTextureMemory((void *)(base_address + ADDR_R_UntrackTextureMemory));
    R_UntrackTextureMemory = orig_R_UntrackTextureMemory;
    if (debug)
        printf("textureswap: %zu places of the game hand the views they replace to its delayed release\n", Site_Count);
}
