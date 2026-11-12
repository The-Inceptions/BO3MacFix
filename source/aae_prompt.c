// All-around Enhancement 4's prompts in the world: the plugin's interactive_prompt component.
//
// The campaign marks things to use with a prompt that sits on them in the world: a name, the button, what it needs
// (scripts give a trigger the hint HINT_INTERACTIVE_PROMPT; the UI side is an element set up with
// setupInteractivePrompt). The mod uses these in zombies for things of its own: the Gambler, the ammo bag, a power
// switch. Its widget shows a prompt in full when the player is near it and hides it otherwise, by two models the
// game keeps for each prompt:
//   nearTo      the player is within reach of it
//   isFarAway   the player is further than a prompt is readable from, and its text is the distance instead
// The game only keeps them in the campaign: at each of the three places that set them it first asks whether the
// session is a campaign one. In zombies they are never set, so every prompt on the map is shown in full all the time.
//
// The plugin replaces those three questions with a yes. It also clears isFarAway for a prompt that has no objective
// (-2), which the game's own code skips. The same is done here, while the mod with the plugin is loaded: each
// question is detoured (detour.h) and the game's own test only runs when the mod isn't there.
//
// Only from the pre-patched executable, like every detour inside a function.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_plugin.h"
#include "aae_prompt.h"
#include "dobby.h"
#include "detour.h"
#include "offsets.h"
#include "prepatch.h"

#define Element_Models 0xe8       // the prompt's UI element: what holds its models, or NULL
#define Element_Objective 0x154   // and the objective it is for
#define Models_Root 0x190         // a model's number (a word), 0 for none
#define Prompt_No_Objective (-2)

// the game's
static uint16_t (*UIModel_Child)(uint16_t model, const char *name); // makes it when it isn't there
static bool (*UIModel_SetBool)(uint16_t model, bool value);
static const char *is_far_away; // the game's own "isFarAway"

bool aae_prompt_clears_far_away(bool mod_loaded, int32_t objective) {
    return mod_loaded && objective == Prompt_No_Objective;
}

// BO3MACFIX_PROMPTDEBUG=1 in the environment: a line the first time the game comes through each place, with the answer
static bool debug;
static bool answer_at(int place, const char *what) {
    static _Atomic unsigned char seen[4];
    bool loaded = aae_plugin_loaded_lately();
    if (debug && !atomic_exchange_explicit(&seen[place], 1, memory_order_relaxed))
        printf("prompts: the game came to %s for the first time, the mod with the plugin is %s\n", what,
               loaded ? "loaded" : "not loaded");
    return loaded;
}

// the three questions "is this the campaign", before nearTo is set, before isFarAway is set and before it is cleared
detour_before_test(Prompt_NearTest) {
    return answer_at(0, "its question before a prompt's nearTo");
}
detour_before_test(Prompt_FarTest) {
    return answer_at(1, "its question before a prompt's isFarAway is set");
}
detour_before_test(Prompt_NotFarTest) {
    return answer_at(2, "its question before a prompt's isFarAway is cleared");
}

// "cmp dword [rbx + 0x154], -2 ; je": a prompt without an objective skips what follows, and with it the place where
// isFarAway is cleared. rbx is the prompt's element.
detour_with_registers(Prompt_NoObjective) {
    const uint8_t *element = (const uint8_t *)registers->rbx, *models;
    int32_t objective;
    uint16_t root;
    memcpy(&objective, element + Element_Objective, sizeof(objective));
    if (!aae_prompt_clears_far_away(answer_at(3, "its test for a prompt without an objective"), objective))
        return;
    memcpy(&models, element + Element_Models, sizeof(models));
    if (models == NULL)
        return;
    memcpy(&root, models + Models_Root, sizeof(root));
    if (root != 0)
        UIModel_SetBool(UIModel_Child(root, is_far_away), false);
}

void aae_prompt_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    if (!prepatch_has_hook(ADDR_Prompt_NearTest) || !prepatch_has_hook(ADDR_Prompt_FarTest) ||
        !prepatch_has_hook(ADDR_Prompt_NotFarTest) || !prepatch_has_hook(ADDR_Prompt_NoObjective)) {
        printf("prompts: not running from the pre-patched executable: All-around Enhancement's prompts in the world "
               "stay as the game keeps them in zombies, always in full\n");
        return;
    }
    aae_plugin_init(game_base_address);
    const char *env = getenv("BO3MACFIX_PROMPTDEBUG");
    debug = env != NULL && atoi(env) != 0;
    UIModel_Child = (void *)(game_base_address + ADDR_UIModel_Child);
    UIModel_SetBool = (void *)(game_base_address + ADDR_UIModel_SetBool);
    is_far_away = (const char *)(game_base_address + ADDR_Prompt_IsFarAwayText);
    install_detour_Prompt_NearTest((void *)(game_base_address + ADDR_Prompt_NearTest),
                                   (void *)(game_base_address + ADDR_Prompt_NearTest_Yes));
    install_detour_Prompt_FarTest((void *)(game_base_address + ADDR_Prompt_FarTest),
                                  (void *)(game_base_address + ADDR_Prompt_FarTest_Yes));
    install_detour_Prompt_NotFarTest((void *)(game_base_address + ADDR_Prompt_NotFarTest),
                                     (void *)(game_base_address + ADDR_Prompt_NotFarTest_Yes));
    install_detour_Prompt_NoObjective((void *)(game_base_address + ADDR_Prompt_NoObjective));
}
