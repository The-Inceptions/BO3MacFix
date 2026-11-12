// All-around Enhancement 4's prompts in the world, kept by distance as its Windows plugin makes the game keep them
// (AAEPrompts). See aae_prompt.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_prompt_init(uint64_t game_base_address, int enabled);

// whether a prompt that has this objective gets its "far away" mark cleared at the place where the game leaves it
// alone
bool aae_prompt_clears_far_away(bool mod_loaded, int32_t objective);

// The glide the plugin gives a prompt that goes back to its place in the world.
// What it keeps for each prompt:
typedef struct {
    float elapsed, duration; // of the glide, in ms
    float start_x, start_y;  // where it began
    float x, y;              // where the prompt is drawn now
    float target_x, target_y; // the prompt's own place when that was last a usable one
    bool has_target;
    bool start_pending; // the prompt wasn't at its own place last time: the next time it is, a glide begins
    bool active;
} aae_prompt_glide_t;

// the prompt isn't drawn at its own place this frame
void aae_prompt_glide_reset(aae_prompt_glide_t *glide);
// The prompt is drawn at its own place this frame, which is (*x, *y) on a screen of width by 720: moves that place
// to where the glide has got to, if there is one. rectangle is the element's own (left, top, right, bottom), and
// drawn_left and drawn_top are where its rectangle was put the last time. speed is the time in ms a glide over 320
// units takes, frame_ms the time since the last frame.
void aae_prompt_glide_step(aae_prompt_glide_t *glide, const float rectangle[4], float drawn_left, float drawn_top, float *x,
                           float *y, float speed, uint32_t frame_ms, float width);
// what is kept for the prompt with this element, NULL for no element. There is room for 128 prompts; after that a
// new one takes the place of one that isn't gliding.
aae_prompt_glide_t *aae_prompt_glide_for(const void *element);

