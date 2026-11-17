// TextureSwapGuard: a texture view the game replaces or drops while the render thread may still be reading it dies a
// few frames later instead of at once.
//
// Aspyr's Direct3D 11 layer on Metal (ASL::DX11Metal) counts references on its objects. The render thread binds a
// picture by handing the context the address of the image's view field; the context reads the view from that field
// and only then, a few instructions and possibly the destruction of the slot's previous view later, takes its own
// reference (PSSetShaderResources 0x10002b7ae: the read at 0x10002b819, the AddRef at 0x10002b87c). Nothing ties
// the two together for the other threads. Several places of the game drop an image's view right away on the main
// thread: the workshop preview swap (Lua's Engine.UpdateModPreviewImage, the CHANGE MAP list) and the default asset
// stubs of a zone unload even release it while the image still points at it and store the new one only after; the
// streamer's view swap (the menus' streamed pictures, the globe's twenty among them) and the image release store
// first and release a moment later. When that release is the last one, the view's destructor waits for the device's
// mutex, which any thread creating a texture holds for the whole upload, while the dead view is still intact. If the
// render thread reads the view in that time, its AddRef brings it back from 0 to 1, it binds and unbinds it, and its
// own release starts a second destruction: one of the two then frees the block a second time. That is the report of
// the user's crash of 2026-11-13 on the render thread (macOS's allocator aborting in Release, called by
// PSSetShaderResources for the old view of a slot), and the menus of a start from Steam run several of these places
// hundreds of times (wip/night/crash-render.md has the whole of it).
//
// The game has the cure itself and uses it in one place: its streamer's mip swap (0x10138ecb7) hands the old view to
// a delayed release (0x10129b485), which untracks and releases it 4 frames later (the dvar 0x275fc2f8 says how many),
// when no field has pointed at it for 4 frames. This guard makes the other places do the same: our hook on the untrack
// function, which each of them calls right before its release, recognises them by their return address, takes a
// reference of its own and queues the view for that delayed release instead of untracking it now; the caller's own
// release then drops only the holder's reference. The view and its texture live 4 frames longer, and are untracked
// then (the streamer's memory budget counts them as long as they exist). Every other caller of the untrack function,
// and every one with the guard off, gets the game's own untrack.
//
// It applies with or without a mod: the preview swap is All-around Enhancement's CHANGE MAP list, the stubs are the
// frontend's unloads of every mod load from the stock MODS menu, the streamer is any menu with streamed pictures.
#include <stdio.h>
#include <stdlib.h>
#include "dobby.h"
#include "offsets.h"
#include "texture_swap_guard.h"

#define Delayed_Category 0 // the texture memory category of every place below, and the one the delayed release untracks

static const texture_swap_site_t sites[] = {
    { ADDR_TextureSwap_PreviewLoadingRet, "the workshop loading picture's swap" },
    { ADDR_TextureSwap_PreviewRet, "the workshop preview picture's swap" },
    { ADDR_TextureSwap_StreamerRet, "the streamer's view swap" },
    { ADDR_TextureSwap_StreamerRet2, "the streamer's view swap (2)" },
    { ADDR_TextureSwap_StreamerRet3, "the streamer's view swap (3)" },
    { ADDR_TextureSwap_ImageFreeRet, "the image release" },
    { ADDR_TextureSwap_ImageFreeRet2, "the image release (second view)" },
    { ADDR_TextureSwap_ImageAssignRet, "a default stub taking its picture" },
    { ADDR_TextureSwap_ImageAssignRet2, "a default stub taking its picture (second view)" },
};
#define Site_Count (sizeof(sites) / sizeof(sites[0]))

static uint64_t base_address;
static bool debug;
static uint32_t handed_over[Site_Count];
static int (*R_UntrackTextureMemory)(void *view, uint64_t category);
static void (*R_DelayedReleaseTexture)(void *view, uint64_t untrack);

const texture_swap_site_t *texture_swap_guard_site(uint64_t return_offset) {
    for (size_t i = 0; i < Site_Count; i++)
        if (sites[i].return_offset == return_offset)
            return &sites[i];
    return NULL;
}

bool texture_swap_guard_delays(const texture_swap_site_t *site, const void *view, uint64_t category) {
    // the game passes the category in esi; only the low half counts, as in the untrack function itself
    return site != NULL && view != NULL && (uint32_t)category == Delayed_Category;
}

bool texture_swap_guard_logs(uint32_t times) {
    return times != 0 && (times <= 3 || (times & (times - 1)) == 0);
}

int texture_swap_guard_untrack(void *view, uint64_t category, uint64_t return_address) {
    const texture_swap_site_t *site = texture_swap_guard_site(return_address - base_address);
    if (!texture_swap_guard_delays(site, view, category))
        return R_UntrackTextureMemory(view, category);
    // the caller releases the holder's reference right after this call: our own reference keeps the view alive through
    // it, and the delayed release gives ours back, untracking the view first, when its frames have passed. AddRef is
    // the second entry of every ASL interface's vtable
    void (*add_ref)(void *) = (*(void (***)(void *))view)[1];
    add_ref(view);
    R_DelayedReleaseTexture(view, 1);
    uint32_t times = __atomic_add_fetch(&handed_over[site - sites], 1, __ATOMIC_RELAXED);
    if (debug && texture_swap_guard_logs(times))
        printf("textureswap: %s handed view %p to the game's delayed release (%u times there)\n", site->function, view,
               times);
    // what the untrack function returns (the entry's count left) isn't read at any of these places
    return 0;
}

// R_UntrackTextureMemory(view, category)
install_hook_name(R_UntrackTextureMemory, int, void *view, uint64_t category) {
    return texture_swap_guard_untrack(view, category, (uint64_t)__builtin_return_address(0));
}

void texture_swap_guard_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_TEXTURESWAPDEBUG");
    debug = env != NULL && atoi(env) != 0;
    R_DelayedReleaseTexture = (void *)(base_address + ADDR_R_DelayedReleaseTexture);
    install_hook_R_UntrackTextureMemory((void *)(base_address + ADDR_R_UntrackTextureMemory));
    R_UntrackTextureMemory = orig_R_UntrackTextureMemory;
    if (debug)
        printf("textureswap: %zu places of the game hand the views they replace to its delayed release\n", Site_Count);
}
