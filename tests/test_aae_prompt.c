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
    stubs_missing_for = -1;
    aae_prompt_init(base, 1);
    CHECK(detours == 4);
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
