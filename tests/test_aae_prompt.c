// aae_prompt.c's handlers against a made-up prompt element and made-up models, and aae_plugin.c's test for the mod
// with the plugin. The thunks that call the handlers in the game are x86_64 code and have a test of their own
// (test_detour.c).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_plugin.c"
#include "../source/aae_prompt.c"

// ---- what the code asks of the rest of BO3MacFix

static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
// which places the pre-patched executable has stubs for, and what was detoured
static bool have_stubs;
static int stubs_missing_for; // one of them missing: its place
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && (int)offset != stubs_missing_for;
}
static struct {
    void *address, *handler, **original;
} detoured[8];
static int detours;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (detours < 8)
        detoured[detours] = (typeof(detoured[0])){ address, fake_func, out_origin_func };
    detours++;
    return 0;
}

// ---- the made-up game: models, and a prompt's element

static struct {
    int child_calls, set_calls;
    uint16_t child_of, set_model;
    const char *child_name;
    bool set_value;
} models;
static uint16_t game_model_child(uint16_t model, const char *name) {
    models.child_calls++;
    models.child_of = model;
    models.child_name = name;
    return 0x4242;
}
static bool game_model_set_bool(uint16_t model, bool value) {
    models.set_calls++;
    models.set_model = model;
    models.set_value = value;
    return true;
}
static const char game_text[] = "isFarAway";

static uint8_t element[0x200], element_models[0x200];
static void set_element(int32_t objective, bool with_models, uint16_t root) {
    const uint8_t *holder = with_models ? element_models : NULL;
    memset(element, 0, sizeof(element));
    memset(element_models, 0, sizeof(element_models));
    memcpy(element + Element_Objective, &objective, sizeof(objective));
    memcpy(element + Element_Models, &holder, sizeof(holder));
    memcpy(element_models + Models_Root, &root, sizeof(root));
}
// the game comes through the place with this element: whether the mark was cleared
static bool no_objective_place(void) {
    detour_registers_t registers;
    memset(&registers, 0x5a, sizeof(registers));
    registers.rbx = (uint64_t)element;
    detour_registers_t before = registers;
    models = (typeof(models)){ 0 };
    handler_Prompt_NoObjective(&registers);
    CHECK(memcmp(&before, &registers, sizeof(registers)) == 0); // the game's registers stay as they are
    if (models.set_calls != 0)
        CHECK(models.child_calls == 1 && models.set_calls == 1 && models.child_name == game_text &&
              models.set_model == 0x4242 && !models.set_value);
    else
        CHECK(models.child_calls == 0);
    return models.set_calls != 0;
}

// ---- the mod with the plugin

static char loaded_name[Mod_Name_Size + 8];
static uint64_t the_time = 5000;
static uint64_t test_now(void) {
    return the_time;
}
static void make_folder(const char *path) {
    char part[4096];
    snprintf(part, sizeof(part), "%s", path);
    for (char *p = part + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(part, 0755);
            *p = '/';
        }
    }
    mkdir(part, 0755);
}
static char plugin_file[4096 + 64];
// a mod folder with or without the plugin in it, loaded or not; and enough time for the next look
static void set_mod(bool plugin, bool loaded) {
    if (plugin)
        close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    else
        unlink(plugin_file);
    memset(loaded_name, 0, sizeof(loaded_name));
    if (loaded)
        snprintf(loaded_name, sizeof(loaded_name), "%s", strrchr(mod_folder, '/') + 1);
    the_time += Plugin_Look_Ms;
}

static void test_plugin(void) {
    // nothing known about the game yet
    loaded_mod = NULL;
    CHECK(!aae_plugin_loaded());
    loaded_mod = loaded_name;
    set_mod(true, true);
    CHECK(aae_plugin_loaded() && aae_plugin_loaded_lately());
    set_mod(false, true);
    CHECK(!aae_plugin_loaded() && !aae_plugin_loaded_lately());
    set_mod(true, false);
    CHECK(!aae_plugin_loaded() && !aae_plugin_loaded_lately());
    // another mod is loaded than the one T7Overcharged was asked for from
    set_mod(true, true);
    loaded_name[0] ^= 1;
    CHECK(!aae_plugin_loaded());
    loaded_name[0] ^= 1;
    CHECK(aae_plugin_loaded());

    // "lately" looks once a second
    set_mod(true, true);
    CHECK(aae_plugin_loaded_lately());
    unlink(plugin_file);
    the_time += Plugin_Look_Ms - 1;
    CHECK(aae_plugin_loaded_lately() && !aae_plugin_loaded());
    the_time += 1;
    CHECK(!aae_plugin_loaded_lately());
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    the_time += Plugin_Look_Ms - 1;
    CHECK(!aae_plugin_loaded_lately() && aae_plugin_loaded());
    the_time += 1;
    CHECK(aae_plugin_loaded_lately());
}

static void test_rule(void) {
    CHECK(aae_prompt_clears_far_away(true, -2));
    CHECK(!aae_prompt_clears_far_away(false, -2));
    CHECK(!aae_prompt_clears_far_away(true, -1) && !aae_prompt_clears_far_away(true, -3) &&
          !aae_prompt_clears_far_away(true, 0) && !aae_prompt_clears_far_away(true, 2) &&
          !aae_prompt_clears_far_away(false, 0));
}

static void test_handlers(void) {
    // with the mod: the three questions are answered with yes, and a prompt without an objective loses its mark
    set_mod(true, true);
    CHECK(handler_Prompt_NearTest() && handler_Prompt_FarTest() && handler_Prompt_NotFarTest());
    set_element(-2, true, 7);
    CHECK(no_objective_place() && models.child_of == 7);
    set_element(-2, true, 0xffff);
    CHECK(no_objective_place() && models.child_of == 0xffff);
    // one with an objective is the game's own business (it clears the mark itself a little later)
    set_element(5, true, 7);
    CHECK(!no_objective_place());
    set_element(-1, true, 7);
    CHECK(!no_objective_place());
    set_element(0, true, 7);
    CHECK(!no_objective_place());
    // an element without models, or without a model of its own
    set_element(-2, false, 7);
    CHECK(!no_objective_place());
    set_element(-2, true, 0);
    CHECK(!no_objective_place());

    // without the mod everything is the game's own
    set_mod(true, false);
    CHECK(!handler_Prompt_NearTest() && !handler_Prompt_FarTest() && !handler_Prompt_NotFarTest());
    set_element(-2, true, 7);
    CHECK(!no_objective_place());
    set_mod(false, true);
    CHECK(!handler_Prompt_NearTest() && !handler_Prompt_FarTest() && !handler_Prompt_NotFarTest());
    CHECK(!no_objective_place());
}

// ---- the glide

// the same frames the plugin's own function was given (see prompt_glide_frames.h), and what that gave
#define Plugin_Lines 97200
#define Plugin_Checksum 0x400fe3511671d98dULL
static struct {
    uint64_t checksum;
    int lines;
} frames_out;
static void frames_byte(uint8_t byte) {
    frames_out.checksum = (frames_out.checksum ^ byte) * 0x100000001b3ULL;
}
static void frames_line(const char *line) {
    for (; *line != '\0'; line++)
        frames_byte((uint8_t)*line);
    frames_byte('\n');
    frames_out.lines++;
}
#include "prompt_glide_frames.h"
static void side_reset(void *e) {
    aae_prompt_glide_reset(aae_prompt_glide_for(e));
}
static void side_step(void *e, float *x, float *y, float speed, uint32_t frame_ms, float width) {
    struct element *made_up = e;
    aae_prompt_glide_step(aae_prompt_glide_for(e), made_up->rectangle, made_up->drawn[0], made_up->drawn[1], x, y, speed,
                          frame_ms, width);
}
static void side_state(void *e, uint8_t out[0x23]) {
    const aae_prompt_glide_t *glide = aae_prompt_glide_for(e);
    const float values[8] = { glide->elapsed, glide->duration, glide->start_x, glide->start_y,
                              glide->x, glide->y, glide->target_x, glide->target_y };
    memcpy(out, values, sizeof(values));
    out[0x20] = glide->has_target, out[0x21] = glide->start_pending, out[0x22] = glide->active;
}

static void test_glide_like_the_plugin(void) {
    frames_out.checksum = 0xcbf29ce484222325ULL, frames_out.lines = 0;
    frames_run();
    CHECK(frames_out.lines == Plugin_Lines);
    CHECK(frames_out.checksum == Plugin_Checksum);
    if (frames_out.checksum != Plugin_Checksum)
        printf("  the glide's frames came out as %#018llx: wip/windows/glide/compare.sh shows where they differ from "
               "the plugin's\n", (unsigned long long)frames_out.checksum);
}

// the same in words, for a prompt with a rectangle of 100 by 40 around its place
static const float around[4] = { -50.0f, -20.0f, 50.0f, 20.0f };
static void test_glide(void) {
    aae_prompt_glide_t glide = { 0 };
    float x = 700.0f, y = 300.0f;
    // nothing begins by itself: a prompt that is at its own place stays there
    aae_prompt_glide_step(&glide, around, 100.0f - 50.0f, 100.0f - 20.0f, &x, &y, 320.0f, 16, 1280.0f);
    CHECK(x == 700.0f && y == 300.0f && !glide.active && !glide.start_pending);

    // it was drawn at (100, 100), the fixed place, and is back at its own (700, 100) now: 600 units at the game's
    // 320 ms for 320 units are 600 ms
    aae_prompt_glide_reset(&glide);
    CHECK(glide.start_pending && !glide.active);
    x = 700.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
    CHECK(glide.active && !glide.start_pending && glide.duration == 600.0f && glide.elapsed == 10.0f &&
          glide.start_x == 100.0f && glide.start_y == 100.0f && glide.has_target && glide.target_x == 700.0f);
    CHECK(x > 100.0f && x < 700.0f && y == 100.0f && x == glide.x);
    // it goes on from where it was put, never back, never more than 4 units a millisecond, and arrives
    float last = x, total = 10.0f;
    int steps = 1;
    while (glide.active && steps < 1000) {
        float own = 700.0f;
        x = own, y = 100.0f;
        aae_prompt_glide_step(&glide, around, last - 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
        total += 10.0f, steps++;
        CHECK(x >= last && x - last <= 40.0f && x <= 700.0f && y == 100.0f);
        last = x;
    }
    CHECK(total == 600.0f && steps == 60 && !glide.active && x == 700.0f);
    // three quarters of the way after 55% of the time
    aae_prompt_glide_reset(&glide);
    x = 700.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 1, 1280.0f);
    last = x;
    for (int ms = 1; ms < 330; ms++) {
        x = 700.0f, y = 100.0f;
        aae_prompt_glide_step(&glide, around, last - 50.0f, 80.0f, &x, &y, 320.0f, 1, 1280.0f);
        last = x;
    }
    CHECK(glide.elapsed == 330.0f && x > 549.0f && x < 551.0f);

    // the prompt's own place moves on meanwhile: the glide ends where that is then
    aae_prompt_glide_reset(&glide);
    float own = 400.0f;
    last = 100.0f;
    for (steps = 0; steps < 1000; steps++) {
        own += 2.0f;
        x = own, y = 100.0f;
        aae_prompt_glide_step(&glide, around, last - 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
        if (!glide.active)
            break;
        CHECK(x <= own && x >= last);
        last = x;
    }
    CHECK(steps > 20 && steps < 40 && x == own && own - last < 40.0f);

    // a short way takes 16 ms at least, a long one 10 s at most
    aae_prompt_glide_reset(&glide);
    x = 101.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 5, 1280.0f);
    CHECK(glide.duration == 16.0f);
    aae_prompt_glide_reset(&glide);
    x = 1100.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 9000.0f, 5, 1280.0f);
    CHECK(glide.duration == 10000.0f);
    // a frame counts as 1 ms at least and 100 at most
    aae_prompt_glide_reset(&glide);
    x = 700.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 0, 1280.0f);
    CHECK(glide.elapsed == 1.0f);
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 5000, 1280.0f);
    CHECK(glide.elapsed == 101.0f);

    // the place stays where the whole rectangle is on the screen: 1280 by 720, or the width given
    aae_prompt_glide_reset(&glide);
    x = 5000.0f, y = -300.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
    CHECK(glide.target_x == 1230.0f && glide.target_y == 20.0f);
    aae_prompt_glide_reset(&glide);
    x = 5000.0f, y = 5000.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 10, 1680.0f);
    CHECK(glide.target_x == 1630.0f && glide.target_y == 700.0f);
    // a width that can't be one counts as 1280
    const float no_widths[] = { 319.0f, 8193.0f, NAN, INFINITY, -1280.0f };
    for (int i = 0; i < 5; i++) {
        aae_prompt_glide_reset(&glide);
        x = 5000.0f, y = 100.0f;
        aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 10, no_widths[i]);
        CHECK(glide.target_x == 1230.0f);
    }
    aae_prompt_glide_reset(&glide);
    x = 5000.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 10, 320.0f);
    CHECK(glide.target_x == 270.0f);
    // a rectangle that can't be one: the place itself stays on the screen
    const float inside_out[4] = { 50.0f, -20.0f, -50.0f, 20.0f };
    aae_prompt_glide_reset(&glide);
    x = 5000.0f, y = -300.0f;
    aae_prompt_glide_step(&glide, inside_out, 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
    CHECK(glide.target_x == 1280.0f && glide.target_y == 0.0f);

    // without a usable time for moves there is no glide, and what was kept is forgotten
    const float no_speeds[] = { 0.0f, -5.0f, 10001.0f, NAN, INFINITY };
    for (int i = 0; i < 5; i++) {
        aae_prompt_glide_reset(&glide);
        x = 700.0f, y = 100.0f;
        aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, no_speeds[i], 10, 1280.0f);
        CHECK(x == 700.0f && y == 100.0f && !glide.active && !glide.start_pending);
    }
    aae_prompt_glide_reset(&glide);
    x = 700.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 10000.0f, 10, 1280.0f);
    CHECK(glide.active);
    // nor from a place that isn't one
    aae_prompt_glide_reset(&glide);
    x = 700.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, NAN, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
    CHECK(x == 700.0f && !glide.active && !glide.start_pending);
    // a prompt whose own place isn't usable for a frame keeps gliding to the last one that was
    aae_prompt_glide_reset(&glide);
    x = 700.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
    float before = x;
    x = NAN, y = 100.0f;
    aae_prompt_glide_step(&glide, around, before - 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
    CHECK(x > before && x < 700.0f && glide.target_x == 700.0f);
    x = 200000.0f, y = 100.0f;
    aae_prompt_glide_step(&glide, around, before - 50.0f, 80.0f, &x, &y, 320.0f, 10, 1280.0f);
    CHECK(x < 700.0f && glide.target_x == 700.0f);
}

static void test_glides_kept(void) {
    // one for each element, the same one every time, none for no element
    static uint8_t prompts[Glide_Prompts + 4];
    memset(&glides, 0, sizeof(glides));
    CHECK(aae_prompt_glide_for(NULL) == NULL);
    aae_prompt_glide_t *first = aae_prompt_glide_for(&prompts[0]);
    CHECK(first != NULL && aae_prompt_glide_for(&prompts[0]) == first && aae_prompt_glide_for(&prompts[1]) != first);
    aae_prompt_glide_reset(first);
    CHECK(aae_prompt_glide_for(&prompts[0])->start_pending);
    // room for 128; after that a new one takes the place of one that isn't gliding or about to
    for (int i = 0; i < Glide_Prompts; i++)
        aae_prompt_glide_reset(aae_prompt_glide_for(&prompts[i]));
    aae_prompt_glide_for(&prompts[5])->start_pending = false;
    aae_prompt_glide_for(&prompts[9])->start_pending = false;
    aae_prompt_glide_for(&prompts[9])->active = true;
    aae_prompt_glide_t *fifth = aae_prompt_glide_for(&prompts[5]);
    CHECK(aae_prompt_glide_for(&prompts[Glide_Prompts]) == fifth && !fifth->start_pending && !fifth->active);
    CHECK(aae_prompt_glide_for(&prompts[9])->active && aae_prompt_glide_for(&prompts[0])->start_pending);
    // all of them busy: the one after the place last given away
    aae_prompt_glide_reset(fifth);
    aae_prompt_glide_t *sixth = aae_prompt_glide_for(&prompts[6]);
    CHECK(aae_prompt_glide_for(&prompts[Glide_Prompts + 1]) == sixth && !sixth->start_pending);
    aae_prompt_glide_reset(sixth);
    aae_prompt_glide_t *seventh = aae_prompt_glide_for(&prompts[7]);
    CHECK(aae_prompt_glide_for(&prompts[Glide_Prompts + 2]) == seventh);
    // the prompt whose place was given away starts anew when it is seen again
    aae_prompt_glide_t *again = aae_prompt_glide_for(&prompts[5]);
    CHECK(again != fifth && !again->start_pending && !again->active);
    memset(&glides, 0, sizeof(glides));
}

// the game comes through its placing code with a prompt's element in r14 and its own values on its frame
static struct {
    uint8_t below[0x100];
    uint8_t top[0x40];
} game_frame;
static void test_glide_handlers(void) {
    static uint8_t prompt[0x200];
    const float rectangle[4] = { -50.0f, -20.0f, 50.0f, 20.0f }, drawn[2] = { 50.0f, 80.0f };
    const float aspect = 1680.0f / 720.0f, speed = 320.0f;
    detour_registers_t registers;
    uint8_t *frame = game_frame.top;
    memset(&glides, 0, sizeof(glides));
    memcpy(prompt + Element_Rectangle, rectangle, sizeof(rectangle));
    memcpy(prompt + Element_Drawn, drawn, sizeof(drawn));

    for (int with_mod = 1; with_mod >= 0; with_mod--) {
        set_mod(true, with_mod);
        memset(&glides, 0, sizeof(glides));
        memset(&game_frame, 0xee, sizeof(game_frame));
        memset(&registers, 0x5a, sizeof(registers));
        registers.r14 = (uint64_t)prompt;
        registers.rbp = (uint64_t)frame;
        registers.r15 = 0xabcdef0000000000ULL | 10; // only the low half is the frame's time
        memcpy(registers.xmm[7], &speed, sizeof(speed));
        memcpy(frame + Frame_Aspect, &aspect, sizeof(aspect));
        detour_registers_t before = registers;
        float place[2] = { 5000.0f, 400.0f }, got[2];
        memcpy(frame + Frame_X, &place[0], sizeof(float));
        memcpy(frame + Frame_Y, &place[1], sizeof(float));

        // kept at the fixed place, then back at its own: the place on the frame is the glide's
        handler_Prompt_NotPlaced(&registers);
        CHECK(memcmp(&before, &registers, sizeof(registers)) == 0);
        CHECK(aae_prompt_glide_for(prompt)->start_pending == (with_mod != 0));
        handler_Prompt_Placed(&registers);
        CHECK(memcmp(&before, &registers, sizeof(registers)) == 0);
        memcpy(&got[0], frame + Frame_X, sizeof(float));
        memcpy(&got[1], frame + Frame_Y, sizeof(float));
        const aae_prompt_glide_t *glide = aae_prompt_glide_for(prompt);
        if (with_mod) {
            // from (100, 100), where it was drawn, towards the right edge of a screen 1680 wide
            CHECK(glide->active && glide->start_x == 100.0f && glide->start_y == 100.0f && glide->target_x == 1630.0f &&
                  glide->target_y == 400.0f && glide->elapsed == 10.0f);
            CHECK(got[0] == glide->x && got[0] > 100.0f && got[0] < 1630.0f && got[1] == glide->y && got[1] > 100.0f &&
                  got[1] < 400.0f);
            // the mod goes while a glide is on its way: the place is the game's own from then on
            set_mod(true, false);
            memcpy(frame + Frame_X, &place[0], sizeof(float));
            memcpy(frame + Frame_Y, &place[1], sizeof(float));
            float glide_x = glide->x;
            handler_Prompt_Placed(&registers);
            memcpy(&got[0], frame + Frame_X, sizeof(float));
            memcpy(&got[1], frame + Frame_Y, sizeof(float));
            CHECK(got[0] == 5000.0f && got[1] == 400.0f && glide->x == glide_x);
        } else {
            CHECK(!glide->active && got[0] == 5000.0f && got[1] == 400.0f);
        }
        // nothing else on the frame was written
        float aspect_after;
        memcpy(&aspect_after, frame + Frame_Aspect, sizeof(aspect_after));
        CHECK(aspect_after == aspect);
        for (int i = 0; i < (int)sizeof(game_frame.below); i++) {
            int offset = i - (int)sizeof(game_frame.below);
            bool written = (offset >= Frame_X && offset < Frame_X + 8) || (offset >= Frame_Aspect && offset < Frame_Aspect + 4);
            if (!written && game_frame.below[i] != 0xee) {
                CHECK(!"something else on the game's frame was written");
                break;
            }
        }
    }
    memset(&glides, 0, sizeof(glides));
}

static bool was_detoured(uint32_t place, void *handler, void **original) {
    for (int i = 0; i < detours && i < 8; i++)
        if (detoured[i].address == (void *)(0x100000000 + (uint64_t)place))
            return detoured[i].handler == handler && detoured[i].original == original;
    return false;
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    // switched off, or not from the pre-patched executable, or from one that lacks a place: nothing is detoured
    have_stubs = true, stubs_missing_for = -1;
    aae_prompt_init(base, 0);
    CHECK(detours == 0);
    have_stubs = false;
    aae_prompt_init(base, 1);
    CHECK(detours == 0);
    have_stubs = true;
    const int places[] = { ADDR_Prompt_NearTest, ADDR_Prompt_FarTest, ADDR_Prompt_NotFarTest, ADDR_Prompt_NoObjective };
    for (int i = 0; i < 4; i++) {
        stubs_missing_for = places[i];
        aae_prompt_init(base, 1);
        CHECK(detours == 0);
    }
    // a pre-patched executable from before the glide: the rest still works
    stubs_missing_for = ADDR_Prompt_Placed;
    aae_prompt_init(base, 1);
    CHECK(detours == 4);
    detours = 0;
    stubs_missing_for = ADDR_Prompt_NotPlaced;
    aae_prompt_init(base, 1);
    CHECK(detours == 4);
    detours = 0;
    stubs_missing_for = -1;
    aae_prompt_init(base, 1);
    CHECK(detours == 6);
    CHECK(was_detoured(ADDR_Prompt_Placed, (void *)handler_Prompt_Placed, &orig_Prompt_Placed));
    CHECK(was_detoured(ADDR_Prompt_NotPlaced, (void *)handler_Prompt_NotPlaced, &orig_Prompt_NotPlaced));
    CHECK(was_detoured(ADDR_Prompt_NearTest, (void *)handler_Prompt_NearTest, &orig_Prompt_NearTest) &&
          yes_Prompt_NearTest == (void *)(base + ADDR_Prompt_NearTest_Yes));
    CHECK(was_detoured(ADDR_Prompt_FarTest, (void *)handler_Prompt_FarTest, &orig_Prompt_FarTest) &&
          yes_Prompt_FarTest == (void *)(base + ADDR_Prompt_FarTest_Yes));
    CHECK(was_detoured(ADDR_Prompt_NotFarTest, (void *)handler_Prompt_NotFarTest, &orig_Prompt_NotFarTest) &&
          yes_Prompt_NotFarTest == (void *)(base + ADDR_Prompt_NotFarTest_Yes));
    CHECK(was_detoured(ADDR_Prompt_NoObjective, (void *)handler_Prompt_NoObjective, &orig_Prompt_NoObjective));
    // each "yes" is behind its own test: mov (5), call (5), test (2) and a jump of 6 or 2 bytes
    CHECK(ADDR_Prompt_NearTest_Yes == ADDR_Prompt_NearTest + 18 && ADDR_Prompt_FarTest_Yes == ADDR_Prompt_FarTest + 14 &&
          ADDR_Prompt_NotFarTest_Yes == ADDR_Prompt_NotFarTest + 14);
    CHECK(UIModel_Child == (void *)(base + ADDR_UIModel_Child) && UIModel_SetBool == (void *)(base + ADDR_UIModel_SetBool) &&
          is_far_away == (const char *)(base + ADDR_Prompt_IsFarAwayText) &&
          loaded_mod == (const char *)(base + ADDR_Mods_LoadedName));
}

int main(void) {
    char template[] = "/tmp/bo3macfix-prompt-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/mods/2631943123", template);
    char folder[4096 + 64];
    snprintf(folder, sizeof(folder), "%s/T7Overcharged/plugins", mod_folder);
    make_folder(folder);
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    plugin_now_ms = test_now;

    test_init(); // first: it points the code at the game, and the other tests at the made-up one
    loaded_mod = loaded_name;
    UIModel_Child = game_model_child;
    UIModel_SetBool = game_model_set_bool;
    is_far_away = game_text;
    test_plugin();
    test_rule();
    test_handlers();
    test_glide_like_the_plugin();
    test_glide();
    test_glides_kept();
    test_glide_handlers();

    unlink(plugin_file);
    char command[4096 + 32];
    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_prompt: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_prompt: all passed\n");
    return 0;
}
