// aae_skeleton.c: the one hook on DObjCalcSkel for every piece that does the plugin's work after the view model's
// skeleton, against a made-up game function and made-up pieces.
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_skeleton.c"

// ---- what the code asks of the rest of BO3MacFix: the hooking function
static struct {
    void *address, *replacement, **original;
} hooked[4];
static int hooks;

// the made-up game's DObjCalcSkel: what it was given, and what it leaves in rax
static struct {
    int calls;
    const uint8_t *object;
    const uint32_t *parts;
    const float *origin;
    int work_calls_before; // how many pieces had done their work when the game's function ran
} game;
static int work_calls;
static const void *hook_returns_to; // where the hook that called the game's function is to return to
static uint64_t game_calc_skeleton(const uint8_t *object, const uint32_t *parts, const float *origin) {
    game.calls++;
    game.object = object, game.parts = parts, game.origin = origin;
    game.work_calls_before = work_calls;
    // the frame above this one is the hook's: its return address (frame pointers are kept, see the Makefile)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-address"
    hook_returns_to = __builtin_return_address(1);
#pragma clang diagnostic pop
    return 0x5ca1ab1e;
}
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 4)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    if (out_origin_func == (void **)&orig_DObjCalcSkel)
        *out_origin_func = (void *)game_calc_skeleton;
    return 0;
}

// ---- two made-up pieces: what each was handed, and in what order they ran
static struct {
    int calls, order, call;
    const uint8_t *object;
} tag_piece, widgets_piece;
static void tag_work(const uint8_t *object, int call) {
    tag_piece.calls++;
    tag_piece.order = ++work_calls;
    tag_piece.object = object, tag_piece.call = call;
}
static void widgets_work(const uint8_t *object, int call) {
    widgets_piece.calls++;
    widgets_piece.order = ++work_calls;
    widgets_piece.object = object, widgets_piece.call = call;
}
static void other_tag_work(const uint8_t *object, int call) {
    (void)object, (void)call;
    tag_piece.calls += 100;
}

static const uint64_t base = 0x100000000;
static const void *first_return(void) {
    return (const void *)(base + ADDR_DObjCalcSkel_ViewModelReturn);
}
static const void *second_return(void) {
    return (const void *)(base + ADDR_DObjCalcSkel_ViewModelReturn2);
}

// everything back to before any piece asked
static void fresh(void) {
    memset(works, 0, sizeof(works));
    memset(view_model_returns, 0, sizeof(view_model_returns));
    installed = false;
    hooks = 0;
    orig_DObjCalcSkel = NULL;
    memset(&game, 0, sizeof(game));
    memset(&tag_piece, 0, sizeof(tag_piece));
    memset(&widgets_piece, 0, sizeof(widgets_piece));
    work_calls = 0;
}
static void forget_calls(void) {
    memset(&game, 0, sizeof(game));
    memset(&tag_piece, 0, sizeof(tag_piece));
    memset(&widgets_piece, 0, sizeof(widgets_piece));
    work_calls = 0;
}

static uint8_t object[0x200];
static const uint32_t parts[12] = { 1, 2, 3 };
static const float origin[3] = { 1.0f, 2.0f, 3.0f };

// the hook as the game calls it, returning to `from`
static uint64_t game_calls_from(const void *from) {
    uint64_t result = orig_DObjCalcSkel(object, parts, origin);
    after_skeleton(object, from);
    return result;
}

static void test_addresses(void) {
    // the game's function and the two calls' return places, as read from its code (the same two the grenade line's
    // piece and the widgets' piece found)
    CHECK(ADDR_DObjCalcSkel == 0xe398de && ADDR_DObjCalcSkel_ViewModelReturn == 0x10b55ea &&
          ADDR_DObjCalcSkel_ViewModelReturn2 == 0x14bb2c5);
    CHECK(AAE_SKELETON_TAG == 0 && AAE_SKELETON_WIDGETS == 1 && AAE_SKELETON_PIECES == 2);
}

static void test_neither(void) {
    // no piece asked: no hook
    fresh();
    CHECK(hooks == 0 && !installed);
    // a piece that isn't one, or no work: nothing either
    aae_skeleton_add(base, AAE_SKELETON_PIECES, tag_work);
    aae_skeleton_add(base, -1, tag_work);
    aae_skeleton_add(base, AAE_SKELETON_TAG, NULL);
    CHECK(hooks == 0 && !installed && works[0] == NULL && works[1] == NULL);
}

static void test_one_piece(void) {
    fresh();
    aae_skeleton_add(base, AAE_SKELETON_WIDGETS, widgets_work);
    // the hook on the game's function, once
    CHECK(hooks == 1 && hooked[0].address == (void *)(base + ADDR_DObjCalcSkel) &&
          hooked[0].replacement == (void *)fake_DObjCalcSkel && hooked[0].original == (void **)&orig_DObjCalcSkel);
    CHECK(view_model_returns[0] == first_return() && view_model_returns[1] == second_return());
    // after the first call: the game's function first, with what it was given, then the piece, told which call
    forget_calls();
    CHECK(game_calls_from(first_return()) == 0x5ca1ab1e);
    CHECK(game.calls == 1 && game.object == object && game.parts == parts && game.origin == origin &&
          game.work_calls_before == 0);
    CHECK(widgets_piece.calls == 1 && widgets_piece.object == object && widgets_piece.call == 0 &&
          tag_piece.calls == 0);
    forget_calls();
    game_calls_from(second_return());
    CHECK(widgets_piece.calls == 1 && widgets_piece.call == 1);
    // after any other call of the function: nothing
    forget_calls();
    CHECK(game_calls_from((const void *)(base + ADDR_DObjCalcSkel_ViewModelReturn + 1)) == 0x5ca1ab1e &&
          game.calls == 1 && widgets_piece.calls == 0);
    game_calls_from((const void *)(base + ADDR_DObjCalcSkel_ViewModelReturn2 - 1));
    game_calls_from(NULL);
    CHECK(widgets_piece.calls == 0);
    // no model: handed on as it is, each piece has its own rule for it
    forget_calls();
    after_skeleton(NULL, first_return());
    CHECK(widgets_piece.calls == 1 && widgets_piece.object == NULL);
    // the grenade line's piece alone
    fresh();
    aae_skeleton_add(base, AAE_SKELETON_TAG, tag_work);
    CHECK(hooks == 1);
    game_calls_from(second_return());
    CHECK(tag_piece.calls == 1 && tag_piece.call == 1 && widgets_piece.calls == 0);
}

static void test_both(void) {
    // in the plugin's order, the grenade line's anchor first, whichever piece asked first; the hook installed once
    fresh();
    aae_skeleton_add(base, AAE_SKELETON_WIDGETS, widgets_work);
    aae_skeleton_add(base, AAE_SKELETON_TAG, tag_work);
    CHECK(hooks == 1);
    game_calls_from(first_return());
    CHECK(tag_piece.calls == 1 && widgets_piece.calls == 1 && tag_piece.order == 1 && widgets_piece.order == 2 &&
          tag_piece.call == 0 && widgets_piece.call == 0);
    forget_calls();
    game_calls_from(second_return());
    CHECK(tag_piece.order == 1 && widgets_piece.order == 2 && tag_piece.call == 1 && widgets_piece.call == 1);
    fresh();
    aae_skeleton_add(base, AAE_SKELETON_TAG, tag_work);
    aae_skeleton_add(base, AAE_SKELETON_WIDGETS, widgets_work);
    CHECK(hooks == 1);
    game_calls_from(first_return());
    CHECK(tag_piece.order == 1 && widgets_piece.order == 2);
    // a piece that asks again: its new work in its place, no second hook
    aae_skeleton_add(base, AAE_SKELETON_TAG, other_tag_work);
    forget_calls();
    game_calls_from(first_return());
    CHECK(hooks == 1 && tag_piece.calls == 100 && widgets_piece.calls == 1);
    // the return places set once, by the first that asked
    CHECK(view_model_returns[0] == first_return() && view_model_returns[1] == second_return());
}

// the hook itself hands on what it was given and gives back what the game's function left
static void test_hook(void) {
    fresh();
    aae_skeleton_add(base, AAE_SKELETON_TAG, tag_work);
    aae_skeleton_add(base, AAE_SKELETON_WIDGETS, widgets_work);
    forget_calls();
    // called from here, which isn't one of the view model's calls: the game's function only
    CHECK(fake_DObjCalcSkel(object, parts, origin) == 0x5ca1ab1e && game.calls == 1 && game.object == object &&
          game.parts == parts && game.origin == origin && tag_piece.calls == 0 && widgets_piece.calls == 0);
    CHECK(fake_DObjCalcSkel(NULL, NULL, NULL) == 0x5ca1ab1e && game.calls == 2 && game.object == NULL &&
          game.parts == NULL && game.origin == NULL);
}

// The hook as the game's call reaches it: through its address (as the game's slot has it, so that it is never copied
// into the caller), always from the same place, so that the place it returns to can be one of the view model's two
static uint64_t (*volatile the_hook)(const uint8_t *, const uint32_t *, const float *) = fake_DObjCalcSkel;
static __attribute__((noinline, disable_tail_calls)) uint64_t through_the_hook(const uint8_t *model,
                                                                             const uint32_t *bits,
                                                                             const float *place) {
    return the_hook(model, bits, place);
}

static void test_return_address(void) {
    fresh();
    aae_skeleton_add(base, AAE_SKELETON_TAG, tag_work);
    aae_skeleton_add(base, AAE_SKELETON_WIDGETS, widgets_work);
    // the place the hook returns to, found out by a first call; from there it isn't one of the two yet
    hook_returns_to = NULL;
    forget_calls();
    CHECK(through_the_hook(object, parts, origin) == 0x5ca1ab1e && hook_returns_to != NULL && tag_piece.calls == 0 &&
          widgets_piece.calls == 0);
    // made the first of the two: the pieces' work, told it was the first
    view_model_returns[0] = hook_returns_to;
    forget_calls();
    CHECK(through_the_hook(object, parts, origin) == 0x5ca1ab1e && game.calls == 1 && tag_piece.calls == 1 &&
          widgets_piece.calls == 1 && tag_piece.call == 0 && widgets_piece.call == 0 && tag_piece.object == object &&
          game.work_calls_before == 0);
    // made the second
    view_model_returns[0] = first_return();
    view_model_returns[1] = hook_returns_to;
    forget_calls();
    through_the_hook(object, parts, origin);
    CHECK(tag_piece.calls == 1 && widgets_piece.calls == 1 && tag_piece.call == 1 && widgets_piece.call == 1);
}

int main(void) {
    test_addresses();
    test_neither();
    test_one_piece();
    test_both();
    test_hook();
    test_return_address();
    if (failures != 0) {
        printf("test_aae_skeleton: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_skeleton: all passed\n");
    return 0;
}
