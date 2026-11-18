#pragma once
#include <stdbool.h>
#include <stdint.h>

// The workshop preview pictures (Lua's Engine.UpdateModPreviewImage: All-around Enhancement's CHANGE MAP list, the
// mods list): the game's loader is replaced by one that decodes the PNG with ImageIO, and with PreviewCache on the
// last few pictures stay loaded, so that showing one again costs nothing (see workshop_preview.c)
void workshop_preview_init(uint64_t game_base_address, int cache_enabled);

#define Preview_Path_Size 1024
#define Preview_Cache_Entries 6           // pictures kept
#define Preview_Cache_Bytes (64ull << 20) // and at most this much of them (4 bytes a pixel)

// a picture file as it is on disk now: a changed file (by a workshop item's update) is another key
typedef struct {
    char path[Preview_Path_Size];
    uint64_t device, inode, size;
    int64_t modified_seconds, modified_nanoseconds;
} preview_key_t;

typedef struct {
    preview_key_t key;
    void *view;           // the texture view, with one reference and one tracking count of the cache's own
    uint64_t bytes;
    uint64_t used;        // when it was last handed out (the cache's clock)
} preview_entry_t;

// the two callers of the loader: the preview picture's swap and the loading picture's
enum { Preview_Picture = 0, Preview_Loading_Picture = 1, Preview_Pictures = 2 };

typedef struct {
    preview_entry_t entries[Preview_Cache_Entries];
    int count;
    uint64_t clock;
    void *held[Preview_Pictures]; // the view last handed to each picture, which it shows now: never given up
    const void *device;           // the Direct3D device the views belong to
} preview_cache_t;

bool preview_key_equal(const preview_key_t *a, const preview_key_t *b);
// the entry with this key, -1 if none
int preview_cache_find(const preview_cache_t *cache, const preview_key_t *key);
// an entry with the key's path but another key (the file changed since), which no picture holds; -1 if none
int preview_cache_stale(const preview_cache_t *cache, const preview_key_t *key);
// whether a view is shown by one of the pictures
bool preview_cache_holds(const preview_cache_t *cache, const void *view);
// the bytes the entries take
uint64_t preview_cache_bytes(const preview_cache_t *cache);
// the entry to give up to make room for a picture of incoming bytes (the least recently used one no picture shows),
// -1 when there is room or nothing can go
int preview_cache_victim(const preview_cache_t *cache, uint64_t incoming);
// the caller of the loader by its return address from the game's base: Preview_Picture, Preview_Loading_Picture or -1
int preview_cache_caller(uint64_t return_offset);
