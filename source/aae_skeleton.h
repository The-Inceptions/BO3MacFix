// The work All-around Enhancement's Windows plugin does right after the game has computed the first person weapon
// model's skeleton, for the pieces that need it (aae_arc.c, aae_vmwidget.c). See aae_skeleton.c.
#pragma once
#include <stdint.h>

// what a piece does once the game has computed the skeleton of `object` at one of the view model's two calls (call: 0
// for the first, 1 for the second); object is the game's, NULL when the game was given none
typedef void (*aae_skeleton_work_t)(const uint8_t *object, int call);

// the pieces, in the order the plugin does their work
enum {
    AAE_SKELETON_TAG,     // arc_prediction_anchor::tag: where the grenade line starts (aae_arc.c)
    AAE_SKELETON_WIDGETS, // viewmodel_ui_anchor: the bones the widgets on the weapon sit on (aae_vmwidget.c)
    AAE_SKELETON_PIECES
};

// A piece asks for its work to be done after the view model's skeleton (at its init). The first one that asks
// installs the hook on DObjCalcSkel; a piece that asks again replaces its work.
void aae_skeleton_add(uint64_t game_base_address, int piece, aae_skeleton_work_t work);
