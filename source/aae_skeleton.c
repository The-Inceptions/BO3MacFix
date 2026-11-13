// The first person weapon model's skeleton, for the pieces that do the plugin's work right after the game has
// computed it.
//
// All-around Enhancement's Windows plugin redirects two of the game's calls of DObjCalcSkel (from where the view
// model is updated, and from where it is posed and drawn) to a function of its own, FUN_180004b00. That one calls the
// game's DObjCalcSkel with what it was given and then does two components' work, in this order: the
// arc_prediction_anchor::tag half (where the grenade line starts: aae_arc.c) and the viewmodel_ui_anchor half (the
// bones the widgets on the weapon sit on: aae_vmwidget.c). It returns what the game's function returned.
//
// On the Mac a hook takes a function, not a call: the pre-patched executable has one slot for DObjCalcSkel, and a
// second hook would replace the first. So the function is hooked here, once, for every piece: after the game's
// function has returned to one of those two calls (told by the return address, Mac 0x1010b55ea and 0x1014bb2c5, the
// calls Windows has at 0x1401abb5c and 0x14099da09), each piece that asked does its work, in the plugin's order. Each
// piece keeps its own conditions (its setting, the mod being loaded, a widget existing) in its own function.
//
// DObjCalcSkel(DObj *object, const uint32_t parts[12], const float origin[3]) takes these three (Mac 0xe398de,
// Windows 0x1422c0480): its five callers set rdi, rsi and rdx and nothing else, one with no origin (NULL); the
// function writes rcx and r8 before it reads them and never reads r9; it reads the parts as 12 words (the bones it
// is asked to compute). None of its callers uses what it leaves in rax; the hook hands that back all the same, as the
// plugin's function does.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "aae_skeleton.h"
#include "dobby.h"
#include "offsets.h"
#include "thread_stats.h"

static aae_skeleton_work_t works[AAE_SKELETON_PIECES];
static const void *view_model_returns[2]; // where the two calls return to
static bool installed;
static int debug; // BO3MACFIX_SKELETONDEBUG

// The game's function has computed the skeleton and is about to return to `from`.
static void after_skeleton(const uint8_t *object, const void *from) {
    int call = from == view_model_returns[0] ? 0 : from == view_model_returns[1] ? 1 : -1;
    if (call < 0)
        return;
    if (debug) {
        static _Atomic int said;
        if ((atomic_fetch_or(&said, 1 << call) & (1 << call)) == 0)
            printf("skeleton: the view model's skeleton call %d is reached, the work of the grenade line is %s, the "
                   "widgets' is %s\n", call + 1, works[AAE_SKELETON_TAG] != NULL ? "done" : "not asked for",
                   works[AAE_SKELETON_WIDGETS] != NULL ? "done" : "not asked for");
    }
    // FrameStats' detail times this work (thread_stats.h); without it nothing else happens here
    phase_start_t start = { 0, 0 };
    if (thread_stats_phases_on)
        start = thread_stats_phase_begin();
    for (int piece = 0; piece < AAE_SKELETON_PIECES; piece++)
        if (works[piece] != NULL)
            works[piece](object, call);
    if (thread_stats_phases_on)
        thread_stats_phase_end(Phase_Library, start);
}

install_hook_name(DObjCalcSkel, uint64_t, const uint8_t *object, const uint32_t *parts, const float *origin) {
    uint64_t result = orig_DObjCalcSkel(object, parts, origin);
    after_skeleton(object, __builtin_return_address(0));
    return result;
}

void aae_skeleton_add(uint64_t game_base_address, int piece, aae_skeleton_work_t work) {
    if (piece < 0 || piece >= AAE_SKELETON_PIECES || work == NULL)
        return;
    works[piece] = work;
    if (installed)
        return;
    installed = true;
    const char *env = getenv("BO3MACFIX_SKELETONDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    view_model_returns[0] = (const void *)(game_base_address + ADDR_DObjCalcSkel_ViewModelReturn);
    view_model_returns[1] = (const void *)(game_base_address + ADDR_DObjCalcSkel_ViewModelReturn2);
    install_hook_DObjCalcSkel((void *)(game_base_address + ADDR_DObjCalcSkel));
}
