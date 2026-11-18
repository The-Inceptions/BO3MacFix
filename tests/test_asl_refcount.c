// asl_refcount.c: the replacement of the Direct3D layer's Release and ReleaseInternal against made-up objects: the
// same counts and results as the game's code for every state one thread meets, exactly one destruction when two
// threads drop the last references together, and the hooks (the test includes the source itself). Built twice: for
// this Mac's own architecture with the sanitizers, and for x86_64 (run under Rosetta) with the game's own four
// functions next to it (tests/asl_refcount_game.s), which it is compared with call by call.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "../source/asl_refcount.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Base 0x100000000ULL

// --- the made-up game --------------------------------------------------------------------------------------------
static void *hooked_at[2], *hooked_with[2];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)out_origin_func;
    if (hooks < 2)
        hooked_at[hooks] = address, hooked_with[hooks] = fake_func;
    hooks++;
    return 0;
}

// an object's IUnknown_Mac part: the vtable, then the external and the internal count; the deleting destructor (entry
// 4) only counts here
typedef struct {
    void *const *vtable;
    uint32_t external, internal;
} object_t;
static atomic_int destroyed;
static void *destroyed_last;
static void game_destroy(void *object) {
    destroyed_last = object;
    atomic_fetch_add(&destroyed, 1);
}
static void unused_entry(void *object) {
    (void)object;
}
static void *const object_vtable[] = { (void *)unused_entry, (void *)unused_entry, (void *)unused_entry,
                                       (void *)unused_entry, (void *)game_destroy, (void *)unused_entry };

// the game's two functions as its code has them (0x1001061d2, 0x100106216), one thread at a time: lower the count,
// read it again, and when it reads 0 and so does the other one, destroy; return what was read (0 if it read 0)
static uint32_t model_release(object_t *object, refcount_kind_t kind) {
    uint32_t *count = kind == Refcount_External ? &object->external : &object->internal;
    uint32_t *other = kind == Refcount_External ? &object->internal : &object->external;
    *count -= 1;
    if (*count != 0)
        return *count;
    if (*other == 0)
        game_destroy(object);
    return 0;
}

#ifdef __x86_64__
// the game's own bytes (tests/asl_refcount_game.s)
extern uint8_t game_refcount_code[];
#define Game_AddRef ((uint32_t(*)(void *))(game_refcount_code + 0x00))
#define Game_Release ((uint32_t(*)(void *))(game_refcount_code + 0x12))
#define Game_AddRefInternal ((uint32_t(*)(void *))(game_refcount_code + 0x44))
#define Game_ReleaseInternal ((uint32_t(*)(void *))(game_refcount_code + 0x56))
#endif

// --- the tests -----------------------------------------------------------------------------------------------------
// every state of the two counts one thread meets, also the ones a release too many leaves: our function gives the
// counts, result and destruction of the game's
static void test_one_thread(void) {
    const uint32_t values[] = { 0, 1, 2, 3, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff };
    const size_t n = sizeof(values) / sizeof(values[0]);
    for (int k = 0; k < 2; k++) {
        refcount_kind_t kind = k == 0 ? Refcount_External : Refcount_Internal;
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < n; j++) {
                object_t ours = { object_vtable, values[i], values[j] }, model = ours;
                atomic_store(&destroyed, 0);
                bool was_zero = false;
                uint32_t left = asl_refcount_release(&ours, kind, &was_zero);
                int ours_destroyed = atomic_load(&destroyed);
                atomic_store(&destroyed, 0);
                uint32_t model_left = model_release(&model, kind);
                CHECK(left == model_left && ours.external == model.external && ours.internal == model.internal);
                CHECK(ours_destroyed == atomic_load(&destroyed));
                CHECK(was_zero == ((kind == Refcount_External ? values[i] : values[j]) == 0));
#ifdef __x86_64__
                object_t game = { object_vtable, values[i], values[j] };
                atomic_store(&destroyed, 0);
                uint32_t game_left = kind == Refcount_External ? Game_Release(&game) : Game_ReleaseInternal(&game);
                CHECK(left == game_left && ours.external == game.external && ours.internal == game.internal);
                CHECK(ours_destroyed == atomic_load(&destroyed));
#endif
            }
        }
    }
    // the destructor gets the object it was called for
    object_t object = { object_vtable, 1, 0 };
    destroyed_last = NULL;
    bool was_zero;
    CHECK(asl_refcount_release(&object, Refcount_External, &was_zero) == 0 && destroyed_last == &object && !was_zero);
}

// two threads drop the last two references together, from a spin barrier: exactly one destruction every time
typedef struct {
    refcount_kind_t kind[2];
    long rounds;
} race_t;
static object_t race_object;
static atomic_long go, ready, done;
static race_t race_case;
static void *race_thread(void *arg) {
    int me = (int)(intptr_t)arg;
    for (long i = 1; i <= race_case.rounds; i++) {
        atomic_fetch_add(&ready, 1);
        while (atomic_load_explicit(&go, memory_order_acquire) != i) {
        }
        bool was_zero;
        asl_refcount_release(&race_object, race_case.kind[me], &was_zero);
        atomic_fetch_add(&done, 1);
    }
    return NULL;
}
static long race(refcount_kind_t a, refcount_kind_t b, uint32_t external, uint32_t internal, long rounds) {
    race_case = (race_t){ { a, b }, rounds };
    atomic_store(&go, 0), atomic_store(&ready, 0);
    pthread_t threads[2];
    for (intptr_t k = 0; k < 2; k++)
        pthread_create(&threads[k], NULL, race_thread, (void *)k);
    long wrong = 0;
    for (long i = 1; i <= rounds; i++) {
        while (atomic_load(&ready) < 2) {
        }
        atomic_store(&ready, 0);
        race_object = (object_t){ object_vtable, external, internal };
        atomic_store(&destroyed, 0), atomic_store(&done, 0);
        atomic_store_explicit(&go, i, memory_order_release);
        while (atomic_load(&done) < 2) {
        }
        wrong += atomic_load(&destroyed) != 1;
    }
    for (int k = 0; k < 2; k++)
        pthread_join(threads[k], NULL);
    return wrong;
}
static void test_two_threads(void) {
    CHECK(race(Refcount_External, Refcount_External, 2, 0, 100000) == 0);
    CHECK(race(Refcount_External, Refcount_Internal, 1, 1, 100000) == 0);
    CHECK(race(Refcount_Internal, Refcount_Internal, 0, 2, 50000) == 0);
}

// the game's AddRef works on one half with a 32-bit atomic; ours on the whole word: two threads taking and dropping
// references on an object that keeps one of each leave the counts exact and destroy nothing
static object_t mixed_object;
static void *mixed_thread(void *arg) {
    refcount_kind_t kind = (refcount_kind_t)(intptr_t)arg;
    for (int i = 0; i < 200000; i++) {
        bool was_zero;
#ifdef __x86_64__
        (kind == Refcount_External ? Game_AddRef : Game_AddRefInternal)(&mixed_object);
#else
        __atomic_add_fetch(kind == Refcount_External ? &mixed_object.external : &mixed_object.internal, 1,
                           __ATOMIC_SEQ_CST);
#endif
        asl_refcount_release(&mixed_object, kind, &was_zero);
    }
    return NULL;
}
static void test_mixed_sizes(void) {
    mixed_object = (object_t){ object_vtable, 1, 1 };
    atomic_store(&destroyed, 0);
    pthread_t threads[3];
    pthread_create(&threads[0], NULL, mixed_thread, (void *)(intptr_t)Refcount_External);
    pthread_create(&threads[1], NULL, mixed_thread, (void *)(intptr_t)Refcount_External);
    pthread_create(&threads[2], NULL, mixed_thread, (void *)(intptr_t)Refcount_Internal);
    for (int k = 0; k < 3; k++)
        pthread_join(threads[k], NULL);
    CHECK(mixed_object.external == 1 && mixed_object.internal == 1 && atomic_load(&destroyed) == 0);
}

static void test_logs(void) {
    CHECK(!asl_refcount_logs(0));
    for (uint32_t i = 1; i <= Log_Lines; i++)
        CHECK(asl_refcount_logs(i));
    CHECK(!asl_refcount_logs(9) && !asl_refcount_logs(15) && asl_refcount_logs(16) && asl_refcount_logs(1u << 31));
}

// the hooks: set only with the setting on, at the two functions, and what they do
static void test_hooks(void) {
    hooks = 0;
    asl_refcount_init(Base, 0);
    CHECK(hooks == 0);
    asl_refcount_init(Base, 1);
    CHECK(hooks == 2 && hooked_at[0] == (void *)(Base + ADDR_ASL_Release) &&
          hooked_with[0] == (void *)fake_ASL_Release && hooked_at[1] == (void *)(Base + ADDR_ASL_ReleaseInternal) &&
          hooked_with[1] == (void *)fake_ASL_ReleaseInternal && base_address == Base);
    CHECK(ADDR_ASL_Release == 0x1061d2 && ADDR_ASL_ReleaseInternal == 0x106216);
    object_t object = { object_vtable, 2, 1 };
    atomic_store(&destroyed, 0);
    CHECK(fake_ASL_Release(&object) == 1 && fake_ASL_Release(&object) == 0 && object.external == 0);
    CHECK(atomic_load(&destroyed) == 0 && releases_too_many == 0);
    CHECK(fake_ASL_ReleaseInternal(&object) == 0 && atomic_load(&destroyed) == 1 && destroyed_last == &object);
    // a release too many: the game's arithmetic, nothing destroyed, counted for the log
    object = (object_t){ object_vtable, 0, 3 };
    CHECK(fake_ASL_Release(&object) == 0xffffffff && object.external == 0xffffffff && object.internal == 3);
    CHECK(releases_too_many == 1 && atomic_load(&destroyed) == 1);
    object = (object_t){ object_vtable, 4, 0 };
    CHECK(fake_ASL_ReleaseInternal(&object) == 0xffffffff && object.internal == 0xffffffff && object.external == 4);
    CHECK(releases_too_many == 2 && atomic_load(&destroyed) == 1);
}

int main(void) {
    test_one_thread();
    test_two_threads();
    test_mixed_sizes();
    test_logs();
    test_hooks();
    if (failures == 0)
        printf("test_asl_refcount: all passed\n");
    return failures == 0 ? 0 : 1;
}
