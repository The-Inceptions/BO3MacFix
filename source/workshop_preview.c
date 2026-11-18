// The workshop preview pictures: Lua's Engine.UpdateModPreviewImage (All-around Enhancement's CHANGE MAP list, the
// mods list) shows a workshop item's previewimage.png on the picture img_t7_mod_preview, and the mod loading screen
// on img_t7_mod_loading.
//
// Aspyr's png to texture conversion is buggy and always crashes, so its loader (0x100e4a26c) is replaced: ImageIO
// decodes the picture, and an RGBA texture and a view of it are made through the game's Direct3D device. The game's
// two swaps (0x100e4a603, 0x100e4a508) call the loader, then untrack and release the picture's old view and store the
// new one. Lua calls UpdateModPreviewImage on every change of focus, and twice for each (the user's log of
// 2026-11-14: every picture "loaded" twice in a row, up to six times while the list is stepped through), so each
// step decoded a PNG of up to 2560x1080 on the main thread and uploaded it, a stutter each time, and made two swaps.
//
// With PreviewCache on, the last Preview_Cache_Entries pictures (at most Preview_Cache_Bytes of them) stay loaded,
// each by its file as it is on disk now: path, device and inode, size and modification time, so a workshop item that
// was updated is loaded anew and never shown from an older copy. A picture asked for again is handed out as it is:
// one reference and one tracking count more, as the swap's fallback to the default picture does it, which the next
// swap's untrack and release take back. The cache holds a reference and a tracking count of its own on each, so the
// texture memory the streamer budgets with counts them while they exist, and a view the cache holds can't die in the
// swap's own release (which drops the picture's reference before the picture gets the new one). A picture is given up
// only when it is neither of the two pictures' current one, so no field still leads the render thread to it; with
// PreviewCache off everything is as before.
//
// All of this runs on the main thread: the loader is only called by the two swaps, the preview's from a Lua function
// of the UI's VM (0x100e0dc9a).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "bo3macnative.h"
#include "dobby.h"
#include "offsets.h"
#include "workshop_preview.h"

// the parts of Direct3D 11 that Aspyr's Metal backend implements, which we need to make textures
typedef struct _D3D11_TEXTURE2D_DESC {
    uint32_t Width, Height, MipLevels, ArraySize, Format, SampleCount, SampleQuality, Usage, BindFlags, CPUAccessFlags,
        MiscFlags;
} D3D11_TEXTURE2D_DESC;
typedef struct _D3D11_SUBRESOURCE_DATA {
    const void *pSysMem;
    uint32_t SysMemPitch, SysMemSlicePitch;
} D3D11_SUBRESOURCE_DATA;
typedef struct _D3D11_SHADER_RESOURCE_VIEW_DESC {
    uint32_t Format, ViewDimension, MostDetailedMip, MipLevels;
    uint64_t unused;
} D3D11_SHADER_RESOURCE_VIEW_DESC;
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#define D3D11_BIND_SHADER_RESOURCE 8
#define D3D11_SRV_DIMENSION_TEXTURE2D 4
#define COM_METHOD(obj, index, type) ((type)((*(void ***)(obj))[index]))
#define Tracking_Category 0 // the texture memory category the swaps untrack with
#define Preview_Return 0xe4a67e // the loader's return in the preview picture's swap (0xe4a603)
#define Loading_Return 0xe4a590 // and in the loading picture's (0xe4a508)

static uint64_t base_address;
static bool cache_enabled, debug;
static preview_cache_t cache;
static uint32_t hits;
static void (*R_TrackTextureMemory)(void *view, D3D11_TEXTURE2D_DESC *desc, int category);
static void (*R_CountTextureMemoryAgain)(void *view, uint64_t category);
static int (*R_UntrackTextureMemory)(void *view, uint64_t category);
static int (*preview_stat)(const char *path, struct stat *info) = stat;

bool preview_key_equal(const preview_key_t *a, const preview_key_t *b) {
    return a->device == b->device && a->inode == b->inode && a->size == b->size &&
           a->modified_seconds == b->modified_seconds && a->modified_nanoseconds == b->modified_nanoseconds &&
           strcmp(a->path, b->path) == 0;
}

int preview_cache_find(const preview_cache_t *c, const preview_key_t *key) {
    for (int i = 0; i < c->count; i++)
        if (preview_key_equal(&c->entries[i].key, key))
            return i;
    return -1;
}

bool preview_cache_holds(const preview_cache_t *c, const void *view) {
    for (int k = 0; k < Preview_Pictures; k++)
        if (c->held[k] == view)
            return true;
    return false;
}

int preview_cache_stale(const preview_cache_t *c, const preview_key_t *key) {
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->entries[i].key.path, key->path) == 0 && !preview_key_equal(&c->entries[i].key, key) &&
            !preview_cache_holds(c, c->entries[i].view))
            return i;
    return -1;
}

uint64_t preview_cache_bytes(const preview_cache_t *c) {
    uint64_t bytes = 0;
    for (int i = 0; i < c->count; i++)
        bytes += c->entries[i].bytes;
    return bytes;
}

int preview_cache_victim(const preview_cache_t *c, uint64_t incoming) {
    if (c->count < Preview_Cache_Entries && preview_cache_bytes(c) + incoming <= Preview_Cache_Bytes)
        return -1;
    int victim = -1;
    for (int i = 0; i < c->count; i++)
        if (!preview_cache_holds(c, c->entries[i].view) && (victim < 0 || c->entries[i].used < c->entries[victim].used))
            victim = i;
    return victim;
}

int preview_cache_caller(uint64_t return_offset) {
    if (return_offset == Preview_Return)
        return Preview_Picture;
    if (return_offset == Loading_Return)
        return Preview_Loading_Picture;
    return -1;
}

// a new texture and view of the picture at path, tracked once (for the picture that will show it); NULL when it can't
// be read or made. The way it always was
static void *load_new(const char *path, uint64_t *bytes) {
    uint32_t width, height;
    uint8_t *pixels = decode_image_rgba(path, &width, &height);
    if (pixels == NULL)
        return NULL;
    void *device = *(void **)(base_address + ADDR_g_d3d11Device);
    D3D11_TEXTURE2D_DESC desc = { width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, 0, D3D11_BIND_SHADER_RESOURCE,
                                  0, 0 };
    D3D11_SUBRESOURCE_DATA data = { pixels, width * 4, width * 4 * height };
    void *texture = NULL;
    void *view = NULL;
    if (device != NULL &&
        COM_METHOD(device, 5, int (*)(void *, D3D11_TEXTURE2D_DESC *, D3D11_SUBRESOURCE_DATA *, void **))(
            device, &desc, &data, &texture) >= 0 &&
        texture != NULL) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view_desc = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D, 0,
                                                      0xFFFFFFFF, 0 };
        if (COM_METHOD(device, 7, int (*)(void *, void *, D3D11_SHADER_RESOURCE_VIEW_DESC *, void **))(
                device, texture, &view_desc, &view) < 0)
            view = NULL;
        if (view != NULL)
            R_TrackTextureMemory(view, &desc, Tracking_Category);
        COM_METHOD(texture, 2, unsigned long (*)(void *))(texture);
    }
    free(pixels);
    printf("workshop image %s (%ux%u): %s\n", path, width, height, view != NULL ? "loaded" : "failed to create texture");
    *bytes = (uint64_t)width * height * 4;
    return view;
}

// the cache's own reference and tracking count go: the picture is gone unless a slot of the render thread still holds
// it (then that slot's release ends it). Only for views no picture shows
static void give_up(int i) {
    void *view = cache.entries[i].view;
    R_UntrackTextureMemory(view, Tracking_Category);
    COM_METHOD(view, 2, unsigned long (*)(void *))(view);
    cache.entries[i] = cache.entries[cache.count - 1];
    cache.count--;
}

static bool key_of(const char *path, preview_key_t *key) {
    struct stat info;
    if (strlen(path) >= sizeof(key->path) || preview_stat(path, &info) != 0)
        return false;
    memset(key, 0, sizeof(*key));
    strcpy(key->path, path);
    key->device = (uint64_t)info.st_dev, key->inode = (uint64_t)info.st_ino, key->size = (uint64_t)info.st_size;
    key->modified_seconds = info.st_mtimespec.tv_sec, key->modified_nanoseconds = info.st_mtimespec.tv_nsec;
    return true;
}

static void *load_cached(const char *path, int caller) {
    const void *device = *(void **)(base_address + ADDR_g_d3d11Device);
    if (device != cache.device) {
        // the views belong to a device that is gone: forgotten, not released (their destructors would use it)
        if (cache.count > 0)
            printf("workshop image: the Direct3D device changed, %d kept pictures forgotten\n", cache.count);
        memset(&cache, 0, sizeof(cache));
        cache.device = device;
    }
    preview_key_t key;
    if (!key_of(path, &key)) {
        uint64_t bytes;
        void *view = load_new(path, &bytes);
        if (caller >= 0)
            cache.held[caller] = view; // not kept: it goes with the picture's next swap
        return view;
    }
    // a copy of the file as it was before a workshop item's update goes, unless a picture still shows it
    int stale;
    while ((stale = preview_cache_stale(&cache, &key)) >= 0) {
        if (debug)
            printf("workshop image %s: changed on disk, the kept one goes\n", path);
        give_up(stale);
    }
    int found = preview_cache_find(&cache, &key);
    if (found >= 0) {
        // the picture's reference and tracking count, as the swap's fallback to the default picture takes them
        void *view = cache.entries[found].view;
        COM_METHOD(view, 1, unsigned long (*)(void *))(view);
        R_CountTextureMemoryAgain(view, Tracking_Category);
        cache.entries[found].used = ++cache.clock;
        if (caller >= 0)
            cache.held[caller] = view;
        hits++;
        if (debug)
            printf("workshop image %s: kept, shown again (%u times from the %d kept, %llu KB)\n", path, hits,
                   cache.count, (unsigned long long)(preview_cache_bytes(&cache) >> 10));
        return view;
    }
    uint64_t bytes = 0;
    void *view = load_new(path, &bytes);
    if (view != NULL) {
        // room for it: the least recently used pictures neither picture shows go. The caller's current picture is
        // shown until the swap has stored the new one, so it stays (the swap releases its reference first)
        int victim;
        while ((victim = preview_cache_victim(&cache, bytes)) >= 0)
            give_up(victim);
        if (cache.count < Preview_Cache_Entries) {
            // the cache's own reference and tracking count
            COM_METHOD(view, 1, unsigned long (*)(void *))(view);
            R_CountTextureMemoryAgain(view, Tracking_Category);
            cache.entries[cache.count++] = (preview_entry_t){ key, view, bytes, ++cache.clock };
        }
    }
    if (caller >= 0)
        cache.held[caller] = view;
    return view;
}

// Aspyr's png to texture conversion code is buggy and always crashes: decode workshop images with ImageIO and make an
// RGBA texture from them ourselves instead
install_hook_name(load_workshop_texture, void *, const char *path) {
    if (!cache_enabled) {
        uint64_t bytes;
        return load_new(path, &bytes);
    }
    return load_cached(path, preview_cache_caller((uint64_t)__builtin_return_address(0) - base_address));
}

void workshop_preview_init(uint64_t game_base_address, int enabled) {
    base_address = game_base_address;
    cache_enabled = enabled != 0;
    const char *env = getenv("BO3MACFIX_PREVIEWDEBUG");
    debug = env != NULL && atoi(env) != 0;
    R_TrackTextureMemory = (void *)(base_address + ADDR_R_TrackTextureMemory);
    R_CountTextureMemoryAgain = (void *)(base_address + ADDR_R_CountTextureMemoryAgain);
    R_UntrackTextureMemory = (void *)(base_address + ADDR_R_UntrackTextureMemory);
    install_hook_load_workshop_texture((void *)(base_address + ADDR_load_workshop_texture));
}
