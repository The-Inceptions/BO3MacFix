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
// The plugin's third change is a glide. The game draws the prompt the player can use right now at a fixed place of
// the screen, and every other one at its thing's place in the world. A prompt that stops being the usable one would
// jump back there. With the plugin it glides: from where it was drawn to its own place, over a time that grows with
// the way (the game's own time for such moves, for every 320 units of it, between 16 ms and 10 s), eased by two
// cubic curves, and never faster than 4 units a millisecond. The plugin keeps what it needs for each prompt by the
// address of its UI element, for 128 of them, and so does this. Its two places: where a prompt's place on the
// screen has been worked out and its rectangle is about to be written, and the other branch, where the prompt is
// kept at the fixed place.
//
// Only from the pre-patched executable, like every detour inside a function.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aae_plugin.h"
#include "aae_prompt.h"
#include "dobby.h"
#include "detour.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are the plugin's, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Element_Rectangle 0x34    // the prompt's UI element: its own rectangle (left, top, right, bottom),
#define Element_Drawn 0x44        // where that was last put on the screen (left, top, right, bottom),
#define Element_Models 0xe8       // what holds its models, or NULL
#define Element_Objective 0x154   // and the objective it is for
// the placing function's own values, from its frame pointer
#define Frame_X (-0x70)       // the prompt's place on the screen, two floats
#define Frame_Y (-0x6c)
#define Frame_Aspect (-0x54)  // the screen's width for a height of 1
#define Screen_Height 720.0f  // what the game counts the UI's height as
#define Models_Root 0x190         // a model's number (a word), 0 for none
#define Prompt_No_Objective (-2)

// the game's
static uint16_t (*UIModel_Child)(uint16_t model, const char *name); // makes it when it isn't there
static bool (*UIModel_SetBool)(uint16_t model, bool value);
static const char *is_far_away; // the game's own "isFarAway"

bool aae_prompt_clears_far_away(bool mod_loaded, int32_t objective) {
    return mod_loaded && objective == Prompt_No_Objective;
}

// BO3MACFIX_PROMPTDEBUG=1 in the environment: a line the first time the game comes through each place, with the
// answer, and one when a glide begins and ends. 2: every prompt's place on the screen as well, twice a second.
static int debug;
static uint8_t *const *debug_cg; // the game's: the first local client's cg_t
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

// ---- the glide

#define Glide_Reference 320.0f // a glide over this far takes the game's time for such moves
#define Glide_Prompts 128

static bool is_number(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000) != 0x7f800000;
}
// the plugin's way of keeping a value within bounds: a comparison with something that isn't a number counts as "not
// above"
static float within(float value, float low, float high) {
    float result = !(low > value) ? value : low;
    return !(value > high) ? result : high;
}
// its C library's hypotf: worked out as doubles
static float way(float x, float y) {
    return (float)sqrt((double)y * (double)y + (double)x * (double)x);
}
static void forget(aae_prompt_glide_t *glide) {
    memset(glide, 0, sizeof(*glide));
}

void aae_prompt_glide_reset(aae_prompt_glide_t *glide) {
    forget(glide);
    glide->start_pending = true;
}

// how far along its way a glide is after this part of its time: two cubic curves, the first to three quarters of the
// way in 55% of the time
static float eased(float time) {
    if (0.55f >= time) {
        float t = within(time / 0.55f, 0.0f, 1.0f), rest = 1.0f - t, three = rest * 3.0f;
        float second = three * t * t * 0.75555563f, first = three * rest * t * 0.5f, third = t * t * t;
        return (first + second + third) * 0.75f;
    }
    float t = within((time - 0.55f) / 0.45f, 0.0f, 1.0f), rest = 1.0f - t, three = rest * 3.0f;
    float second = three * t * t, first = three * rest * t * 0.6f, third = t * t * t;
    return (first + second + third) * 0.25f + 0.75f;
}

void aae_prompt_glide_step(aae_prompt_glide_t *glide, const float rectangle[4], float drawn_left, float drawn_top, float *x,
                           float *y, float speed, uint32_t frame_ms, float width) {
    float left = rectangle[0], top = rectangle[1];
    float before_x = drawn_left - left, before_y = drawn_top - top; // where the prompt was drawn the last time
    float own_x = *x, own_y = *y;
    if (!is_number(speed) || !(speed > 0.0f) || speed > 10000.0f) {
        forget(glide);
        return;
    }
    if (!is_number(width) || width < Glide_Reference || !(8192.0f >= width))
        width = 1280.0f;
    // how far the prompt's place may go for its rectangle to stay on the screen
    float low_x = 0.0f, low_y = 0.0f, high_x = width, high_y = Screen_Height;
    if (is_number(left) && is_number(top) && is_number(rectangle[2]) && is_number(rectangle[3])) {
        float right = rectangle[2], bottom = rectangle[3];
        if (!(right < left) && !(bottom < top) && !(width < right - left) && !(Screen_Height < bottom - top) &&
            !(32768.0f < fabsf(left)) && !(32768.0f < fabsf(right)) && !(32768.0f < fabsf(top)) &&
            !(32768.0f < fabsf(bottom))) {
            high_x = width - right;
            low_x = -left;
            high_y = Screen_Height - bottom;
            low_y = -top;
        }
    }
    bool usable = is_number(own_x) && is_number(own_y) && 100000.0f > fabsf(own_x) && 100000.0f > fabsf(own_y);
    float to_x = usable ? within(own_x, low_x, high_x) : 0.0f, to_y = usable ? within(own_y, low_y, high_y) : 0.0f;

    if (glide->start_pending) {
        glide->start_pending = false;
        if (!is_number(before_x) || !is_number(before_y)) {
            forget(glide);
            return;
        }
        glide->active = true;
        glide->elapsed = 0.0f;
        glide->start_x = glide->x = within(before_x, low_x, high_x);
        glide->start_y = glide->y = within(before_y, low_y, high_y);
        float distance = usable ? way(to_x - glide->start_x, to_y - glide->start_y) : Glide_Reference;
        glide->duration = within(distance * speed / Glide_Reference, 16.0f, 10000.0f);
    }
    if (!glide->active)
        return;

    float frame = within((float)frame_ms, 1.0f, 100.0f);
    float elapsed = frame + glide->elapsed;
    elapsed = elapsed < glide->duration ? elapsed : glide->duration;
    glide->elapsed = elapsed;
    if (elapsed >= glide->duration) {
        // there: the prompt is at its own place from this frame on
        forget(glide);
        return;
    }
    if (usable) {
        glide->target_x = to_x;
        glide->target_y = to_y;
        glide->has_target = true;
    }
    float now_x = glide->x, now_y = glide->y;
    float goal_x = glide->has_target ? glide->target_x : glide->x, goal_y = glide->has_target ? glide->target_y : glide->y;
    float part = eased(within(elapsed / glide->duration, 0.0f, 1.0f));
    float move_x = (goal_x - glide->start_x) * part + glide->start_x - now_x;
    float move_y = (goal_y - glide->start_y) * part + glide->start_y - now_y;
    // not more than 4 units a millisecond
    float distance = way(move_x, move_y), most = frame * 4.0f;
    float share = distance > most ? most / distance : 1.0f;
    glide->x = within(share * move_x + now_x, low_x, high_x);
    glide->y = within(share * move_y + now_y, low_y, high_y);
    *x = glide->x;
    *y = glide->y;
}

// The prompts' glides by their elements, in the order they were first seen. When all places are taken, a new prompt
// gets the first one from "next" on that isn't gliding or about to, or the one at "next" itself.
static struct {
    const void *element[Glide_Prompts];
    aae_prompt_glide_t glide[Glide_Prompts];
    unsigned count, next;
} glides;

aae_prompt_glide_t *aae_prompt_glide_for(const void *element) {
    if (element == NULL)
        return NULL;
    for (unsigned i = 0; i < glides.count; i++)
        if (glides.element[i] == element)
            return &glides.glide[i];
    unsigned place = glides.count;
    if (glides.count < Glide_Prompts) {
        glides.count++;
    } else {
        place = glides.next;
        for (unsigned i = 0; i < Glide_Prompts; i++) {
            unsigned candidate = (glides.next + i) % Glide_Prompts;
            if (!glides.glide[candidate].active && !glides.glide[candidate].start_pending) {
                place = candidate;
                break;
            }
        }
        glides.next = (place + 1) % Glide_Prompts;
    }
    glides.element[place] = element;
    forget(&glides.glide[place]);
    return &glides.glide[place];
}

// "movups xmm0, [r14 + 0x34]": the prompt (its element in r14) has its place on the screen, in the function's own
// values, and its rectangle is about to be written from it
detour_with_registers(Prompt_Placed) {
    if (!aae_plugin_loaded_lately())
        return;
    const uint8_t *element = (const uint8_t *)registers->r14;
    uint8_t *frame = (uint8_t *)registers->rbp;
    float rectangle[4], drawn[2], place[2], speed, aspect;
    memcpy(rectangle, element + Element_Rectangle, sizeof(rectangle));
    memcpy(drawn, element + Element_Drawn, sizeof(drawn));
    memcpy(&place[0], frame + Frame_X, sizeof(float));
    memcpy(&place[1], frame + Frame_Y, sizeof(float));
    memcpy(&aspect, frame + Frame_Aspect, sizeof(aspect));
    memcpy(&speed, registers->xmm[7], sizeof(speed)); // the game's time for such moves, which it has just read
    aae_prompt_glide_t *glide = aae_prompt_glide_for(element);
    bool was_gliding = glide->active;
    if (debug >= 2) {
        static uint64_t said[Glide_Prompts];
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
        size_t which = (size_t)(glide - glides.glide);
        if (now - said[which] >= 500) {
            said[which] = now;
            float player[3] = { 0 };
            const uint8_t *cg = debug_cg != NULL ? *debug_cg : NULL;
            if (cg != NULL)
                memcpy(player, cg + 0x11a8d0 + 0x30, sizeof(player)); // the predicted player state's origin
            printf("prompts: prompt %zu is at %.0f %.0f on a screen %.0f wide, the player at %.0f %.0f %.0f\n", which,
                   place[0], place[1], aspect * Screen_Height, player[0], player[1], player[2]);
        }
    }
    aae_prompt_glide_step(glide, rectangle, drawn[0], drawn[1], &place[0], &place[1], speed, (uint32_t)registers->r15,
                          aspect * Screen_Height);
    if (debug && glide->active != was_gliding) {
        if (glide->active)
            printf("prompts: a prompt glides back to its own place, from %.0f %.0f to %.0f %.0f in %.0f ms\n",
                   glide->start_x, glide->start_y, glide->target_x, glide->target_y, glide->duration);
        else
            printf("prompts: the prompt is at its own place again, %.0f %.0f\n", place[0], place[1]);
    }
    memcpy(frame + Frame_X, &place[0], sizeof(float));
    memcpy(frame + Frame_Y, &place[1], sizeof(float));
}

// "movss xmm5, [rbp - 0x54]": the other branch, the prompt (r14) is kept at the fixed place
detour_with_registers(Prompt_NotPlaced) {
    if (!aae_plugin_loaded_lately())
        return;
    aae_prompt_glide_t *glide = aae_prompt_glide_for((const void *)registers->r14);
    if (debug && !glide->start_pending)
        printf("prompts: prompt %zu is the one to use now, drawn at the fixed place\n", (size_t)(glide - glides.glide));
    aae_prompt_glide_reset(glide);
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
    debug = env != NULL ? atoi(env) : 0;
    debug_cg = (uint8_t *const *)(game_base_address + ADDR_cgArray);
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
    // the glide is a thing of its own: a copy of the executable from before it had these two still gets the rest
    if (prepatch_has_hook(ADDR_Prompt_Placed) && prepatch_has_hook(ADDR_Prompt_NotPlaced)) {
        install_detour_Prompt_Placed((void *)(game_base_address + ADDR_Prompt_Placed));
        install_detour_Prompt_NotPlaced((void *)(game_base_address + ADDR_Prompt_NotPlaced));
    }
}
