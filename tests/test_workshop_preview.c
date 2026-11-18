// workshop_preview.c: the loader of the workshop preview pictures and its cache, against a made-up Direct3D device,
// texture memory table, decoder and files, driven through a copy of the game's swap (the test includes the source).
// After every swap each view must have one reference and one tracking count for each picture that shows it and one
// for the cache if it keeps it, and a view must be destroyed exactly when nothing has it any more.
#include <stdio.h>
#include <string.h>
#include "../source/workshop_preview.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// --- the made-up game --------------------------------------------------------------------------------------------
static int hooks;
static void *hooked_with;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)out_origin_func;
    hooks++, hooked_with = fake_func;
    return 0;
}

// textures and views: counted references; a view holds its texture, and is tracked in the texture memory table
#define Most_Objects 256
typedef struct object {
    void *const *vtable;
    int refs, tracked, destroyed;
    struct object *texture; // a view's
    uint32_t width, height;
} object_t;
static object_t objects[Most_Objects];
static int object_count;
static void object_add_ref(void *opaque) {
    object_t *object = opaque;
    CHECK(object->refs > 0 && !object->destroyed); // never a reference on a dead object
    object->refs++;
}
static void object_release(void *opaque) {
    object_t *object = opaque;
    CHECK(object->refs > 0);
    if (--object->refs == 0) {
        object->destroyed++;
        if (object->texture != NULL)
            object_release(object->texture);
    }
}
static void unused_entry(void) {
}
static void *const object_vtable[] = { (void *)unused_entry, (void *)object_add_ref, (void *)object_release };
static object_t *new_object(uint32_t width, uint32_t height) {
    object_t *object = &objects[object_count++];
    *object = (object_t){ object_vtable, 1, 0, 0, NULL, width, height };
    return object;
}

// the device: CreateTexture2D (entry 5) and CreateShaderResourceView (entry 7)
static int textures_made, upload_bytes;
static int device_create_texture(void *device, D3D11_TEXTURE2D_DESC *desc, D3D11_SUBRESOURCE_DATA *data,
                                 void **texture) {
    (void)device;
    CHECK(desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM && desc->BindFlags == D3D11_BIND_SHADER_RESOURCE);
    CHECK(data->SysMemPitch == desc->Width * 4 && data->pSysMem != NULL);
    textures_made++, upload_bytes += (int)(desc->Width * desc->Height * 4);
    *texture = new_object(desc->Width, desc->Height);
    return 0;
}
static int device_create_view(void *device, void *texture, D3D11_SHADER_RESOURCE_VIEW_DESC *desc, void **view) {
    (void)device;
    CHECK(desc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
    object_t *made = new_object(((object_t *)texture)->width, ((object_t *)texture)->height);
    made->texture = texture;
    ((object_t *)texture)->refs++; // the view's hold on its texture
    *view = made;
    return 0;
}
static void *const device_vtable[] = { 0, 0, 0, 0, 0, (void *)device_create_texture, 0, (void *)device_create_view };
static void *const made_up_device[] = { (void *)device_vtable };
static void *const other_device[] = { (void *)device_vtable };
static const void *device_slot; // the game's g_d3d11Device

// the texture memory table: a view tracked with its size, counted again, untracked
static void game_track(void *view, D3D11_TEXTURE2D_DESC *desc, int category) {
    CHECK(category == 0 && desc->Width == ((object_t *)view)->width);
    CHECK(((object_t *)view)->tracked == 0);
    ((object_t *)view)->tracked = 1;
}
static void game_count_again(void *view, uint64_t category) {
    CHECK(category == 0);
    if (((object_t *)view)->tracked > 0) // as the game's: an entry that isn't there isn't counted
        ((object_t *)view)->tracked++;
}
static int game_untrack(void *view, uint64_t category) {
    CHECK(category == 0);
    object_t *object = view;
    return object->tracked > 0 ? --object->tracked : 0;
}

// the files: a path, its size and modification time, and the picture's size; the decoder counts its work
typedef struct {
    const char *path;
    uint64_t inode, size;
    int64_t modified;
    uint32_t width, height;
    bool missing, broken;
} file_t;
static file_t files[16];
static int file_count, decodes;
static file_t *file_named(const char *path) {
    for (int i = 0; i < file_count; i++)
        if (strcmp(files[i].path, path) == 0)
            return &files[i];
    return NULL;
}
static int fake_stat(const char *path, struct stat *info) {
    file_t *file = file_named(path);
    if (file == NULL || file->missing)
        return -1;
    memset(info, 0, sizeof(*info));
    info->st_dev = 1, info->st_ino = file->inode, info->st_size = (off_t)file->size;
    info->st_mtimespec.tv_sec = file->modified, info->st_mtimespec.tv_nsec = 5;
    return 0;
}
uint8_t *decode_image_rgba(const char *path, uint32_t *width, uint32_t *height) {
    file_t *file = file_named(path);
    decodes++;
    if (file == NULL || file->missing || file->broken)
        return NULL;
    *width = file->width, *height = file->height;
    return calloc(1, 16);
}
static file_t *add_file(const char *path, uint32_t width, uint32_t height) {
    file_t *file = &files[file_count++];
    *file = (file_t){ path, (uint64_t)file_count, (uint64_t)width * 7, 1000, width, height, false, false };
    return file;
}

// the game's two pictures and its swap (0x100e4a603): load, untrack and release the old view, the default picture's
// view if the load failed, store
static object_t *default_view;
static object_t *picture[Preview_Pictures];
static void swap(int which, const char *path, bool through_hook) {
    void *view = through_hook ? fake_load_workshop_texture(path) : load_cached(path, which);
    game_untrack(picture[which], 0);
    object_release(picture[which]);
    if (view == NULL) {
        view = default_view;
        game_count_again(view, 0);
        object_add_ref(view);
    }
    picture[which] = view;
}

// every view: as many references and tracking counts as pictures show it plus the cache's; dead when none
static void check_counts(void) {
    for (int i = 0; i < object_count; i++) {
        object_t *object = &objects[i];
        if (object->texture == NULL || object == default_view)
            continue;
        int shown = (picture[0] == object) + (picture[1] == object);
        int kept = 0;
        for (int e = 0; e < cache.count; e++)
            kept += cache.entries[e].view == object;
        CHECK(kept <= 1);
        if (shown + kept == 0) {
            CHECK(object->destroyed == 1 && object->refs == 0 && object->tracked == 0);
            CHECK(object->texture->destroyed == 1 && object->texture->refs == 0);
        } else {
            CHECK(object->texture->destroyed == 0 && object->texture->refs == 1);
            CHECK(object->destroyed == 0 && object->refs == shown + kept && object->tracked == shown + kept);
        }
    }
    CHECK(cache.count <= Preview_Cache_Entries);
}

static void set_up(bool enabled) {
    memset(objects, 0, sizeof(objects));
    object_count = file_count = decodes = textures_made = upload_bytes = 0;
    memset(&cache, 0, sizeof(cache));
    hits = 0;
    device_slot = made_up_device;
    workshop_preview_init((uint64_t)&device_slot - ADDR_g_d3d11Device, enabled);
    R_TrackTextureMemory = game_track;
    R_CountTextureMemoryAgain = game_count_again;
    R_UntrackTextureMemory = game_untrack;
    preview_stat = fake_stat;
    // the pictures as the zone made them: views of their own, tracked
    default_view = new_object(8, 8);
    default_view->texture = new_object(8, 8), default_view->tracked = 1, default_view->refs = 1000;
    for (int k = 0; k < Preview_Pictures; k++) {
        picture[k] = new_object(4, 4);
        picture[k]->texture = new_object(4, 4), picture[k]->tracked = 1;
    }
}

// --- the tests -----------------------------------------------------------------------------------------------------
static void test_rules(void) {
    preview_key_t a = { "a.png", 1, 2, 3, 4, 5 }, b = a;
    CHECK(preview_key_equal(&a, &b));
    b.modified_nanoseconds = 6;
    CHECK(!preview_key_equal(&a, &b));
    b = a, b.size = 4;
    CHECK(!preview_key_equal(&a, &b));
    b = a, b.inode = 9;
    CHECK(!preview_key_equal(&a, &b));
    b = a, b.device = 9;
    CHECK(!preview_key_equal(&a, &b));
    b = a, b.modified_seconds = 9;
    CHECK(!preview_key_equal(&a, &b));
    b = a, strcpy(b.path, "b.png");
    CHECK(!preview_key_equal(&a, &b));
    CHECK(preview_cache_caller(0xe4a67e) == Preview_Picture && preview_cache_caller(0xe4a590) == Preview_Loading_Picture);
    CHECK(preview_cache_caller(0xe4a67f) == -1 && preview_cache_caller(0xe4a58b) == -1 && preview_cache_caller(0) == -1);
}

// without the cache: every swap decodes and makes a texture, the old view dies in the swap, as before
static void test_off(void) {
    set_up(false);
    CHECK(hooks > 0 && hooked_with == (void *)fake_load_workshop_texture);
    add_file("a.png", 100, 50);
    swap(0, "a.png", true);
    swap(0, "a.png", true);
    swap(0, "a.png", true);
    CHECK(decodes == 3 && textures_made == 3 && cache.count == 0);
    int alive = 0;
    for (int i = 0; i < object_count; i++)
        alive += objects[i].texture != NULL && objects[i].destroyed == 0 && &objects[i] != default_view;
    CHECK(alive == 2); // the picture's current view and the loading picture's own
    check_counts();
}

// the user's list: each picture asked for twice, stepped back and forth
static void test_stepping(void) {
    set_up(true);
    add_file("a.png", 1280, 540), add_file("b.png", 1440, 607), add_file("c.png", 600, 340);
    const char *steps[] = { "a.png", "a.png", "b.png", "b.png", "a.png", "a.png", "c.png", "c.png", "b.png", "b.png" };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        swap(Preview_Picture, steps[i], false);
        check_counts();
    }
    CHECK(decodes == 3 && textures_made == 3 && cache.count == 3 && hits == 7);
    CHECK(cache.held[Preview_Picture] == picture[Preview_Picture]);
    // the loading picture shows one the preview shows too
    swap(Preview_Loading_Picture, "b.png", false);
    check_counts();
    CHECK(picture[0] == picture[1] && decodes == 3);
    // and through the hook, from an unknown place: shown again all the same
    swap(Preview_Picture, "a.png", true);
    check_counts();
    CHECK(decodes == 3);
}

// more pictures than kept: the least recently used one nobody shows goes, never a shown one
static void test_room(void) {
    set_up(true);
    char names[10][8];
    for (int i = 0; i < 10; i++) {
        snprintf(names[i], sizeof(names[i]), "%d.png", i);
        add_file(names[i], 64, 64);
    }
    swap(Preview_Loading_Picture, names[0], false); // the loading picture keeps 0 all along
    for (int i = 1; i < 10; i++) {
        swap(Preview_Picture, names[i], false);
        check_counts();
        CHECK(cache.count <= Preview_Cache_Entries);
    }
    CHECK(cache.count == Preview_Cache_Entries && preview_cache_find(&cache, &(preview_key_t){ 0 }) == -1);
    preview_key_t key;
    CHECK(key_of(names[0], &key) && preview_cache_find(&cache, &key) >= 0); // shown: kept
    CHECK(key_of(names[1], &key) && preview_cache_find(&cache, &key) < 0);  // the oldest: gone
    CHECK(key_of(names[9], &key) && preview_cache_find(&cache, &key) >= 0);
    // back to 8: kept, nothing decoded
    int before = decodes;
    swap(Preview_Picture, names[8], false);
    CHECK(decodes == before);
    check_counts();
    // kept now: 0 (the loading picture shows it), 5, 6, 7, 9 and 8 (shown); the oldest is 5. Shown again, 5 counts as
    // recent, so 6 is the one to go when 1 comes back (with 9 shown by then)
    CHECK(key_of(names[5], &key) && preview_cache_find(&cache, &key) >= 0);
    swap(Preview_Picture, names[5], false);
    swap(Preview_Picture, names[9], false); // and away from it again, so that only its age keeps it
    CHECK(decodes == before);
    swap(Preview_Picture, names[1], false); // not kept: decoded, and room made
    CHECK(decodes == before + 1);
    CHECK(key_of(names[5], &key) && preview_cache_find(&cache, &key) >= 0);
    CHECK(key_of(names[6], &key) && preview_cache_find(&cache, &key) < 0);
    CHECK(key_of(names[7], &key) && preview_cache_find(&cache, &key) >= 0);
    check_counts();
}

// the caller's current picture is the least recently used one when the cache is full: it stays through the swap's
// release (which drops the picture's reference before the picture gets the new one)
static void test_current_stays(void) {
    set_up(true);
    char names[8][8];
    for (int i = 0; i < 8; i++) {
        snprintf(names[i], sizeof(names[i]), "%d.png", i);
        add_file(names[i], 32, 32);
    }
    swap(Preview_Picture, names[0], false);
    object_t *shown = picture[Preview_Picture];
    for (int i = 1; i < 6; i++)
        swap(Preview_Loading_Picture, names[i], false);
    // full: 0 (shown by the preview, the oldest), 1..5; now the preview asks for 6
    CHECK(cache.count == Preview_Cache_Entries);
    void *view = load_cached(names[6], Preview_Picture);
    CHECK(shown->destroyed == 0 && shown->refs >= 2); // still the picture's and the cache's
    // the rest of the swap
    game_untrack(picture[Preview_Picture], 0);
    object_release(picture[Preview_Picture]);
    CHECK(shown->destroyed == 0);
    picture[Preview_Picture] = view;
    check_counts();
}

// more bytes than kept: big pictures make room for each other
static void test_bytes(void) {
    set_up(true);
    add_file("big1.png", 4096, 2048), add_file("big2.png", 4096, 2048), add_file("big3.png", 4096, 2048);
    swap(Preview_Picture, "big1.png", false);
    swap(Preview_Picture, "big2.png", false);
    CHECK(cache.count == 2 && preview_cache_bytes(&cache) == 2ull * 4096 * 2048 * 4);
    swap(Preview_Picture, "big3.png", false);
    check_counts();
    CHECK(cache.count == 1 + 1 && preview_cache_bytes(&cache) <= Preview_Cache_Bytes + 4096ull * 2048 * 4);
    preview_key_t key;
    CHECK(key_of("big1.png", &key) && preview_cache_find(&cache, &key) < 0);
}

// an updated workshop item: loaded anew, never shown from the old copy; the old copy goes when nobody shows it
static void test_changed_file(void) {
    set_up(true);
    file_t *file = add_file("a.png", 100, 100);
    add_file("b.png", 50, 50);
    swap(Preview_Picture, "a.png", false);
    object_t *old = picture[Preview_Picture];
    file->modified = 2000, file->width = 120; // updated while shown
    void *fresh = load_cached("a.png", Preview_Picture);
    CHECK(old->refs == 2 && old->destroyed == 0); // the picture's and the cache's, through the load
    game_untrack(picture[Preview_Picture], 0);
    object_release(picture[Preview_Picture]);
    CHECK(old->destroyed == 0);
    picture[Preview_Picture] = fresh;
    CHECK(decodes == 2 && picture[Preview_Picture] != old && picture[Preview_Picture]->width == 120);
    check_counts();
    // the old copy is still kept (it was shown during that load); the next load of the path lets it go
    swap(Preview_Picture, "b.png", false);
    swap(Preview_Picture, "a.png", false);
    CHECK(decodes == 3 && picture[Preview_Picture]->width == 120 && old->destroyed == 1);
    check_counts();
    file->size = 1, file->modified = 2000;
    swap(Preview_Picture, "a.png", false);
    CHECK(decodes == 4);
    file->inode = 77;
    swap(Preview_Picture, "a.png", false);
    CHECK(decodes == 5);
    check_counts();
}

// a file that can't be looked at or read: loaded as before or the default picture, nothing kept
static void test_missing_and_broken(void) {
    set_up(true);
    file_t *missing = add_file("gone.png", 10, 10);
    missing->missing = true;
    file_t *broken = add_file("broken.png", 10, 10);
    broken->broken = true;
    add_file("a.png", 10, 10);
    swap(Preview_Picture, "a.png", false);
    swap(Preview_Picture, "gone.png", false);
    CHECK(picture[Preview_Picture] == default_view && cache.held[Preview_Picture] == NULL && cache.count == 1);
    swap(Preview_Picture, "broken.png", false);
    CHECK(picture[Preview_Picture] == default_view && cache.count == 1 && decodes == 3);
    check_counts();
    swap(Preview_Picture, "a.png", false);
    CHECK(decodes == 3);
    check_counts();
}

// a new Direct3D device: the kept views are its old one's, forgotten without a release
static void test_new_device(void) {
    set_up(true);
    add_file("a.png", 10, 10);
    swap(Preview_Picture, "a.png", false);
    object_t *kept = picture[Preview_Picture];
    int refs = kept->refs;
    device_slot = other_device;
    swap(Preview_Picture, "a.png", false);
    CHECK(decodes == 2 && cache.count == 1 && cache.device == other_device && kept->refs == refs - 1);
}

int main(void) {
    test_rules();
    test_off();
    test_stepping();
    test_room();
    test_current_stays();
    test_bytes();
    test_changed_file();
    test_missing_and_broken();
    test_new_device();
    if (failures == 0)
        printf("test_workshop_preview: all passed\n");
    return failures == 0 ? 0 : 1;
}
