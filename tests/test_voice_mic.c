// voice_mic.c: VoiceMicrophone 0 refuses the voice system's start of recording, without calling Steam; 1 hooks nothing
// (the test includes the source itself, against a made-up game).
#include <stdio.h>
#include <string.h>
#include "../source/voice_mic.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// the made-up game: the capture layer's start, which would have Steam record
static int steam_starts;
static int game_start(void *capture) {
    (void)capture;
    steam_starts++;
    return 0;
}
static void *hooked_at, *hooked_with;
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    hooked_at = address, hooked_with = fake_func, hooks++;
    *out_origin_func = (void *)game_start;
    return 0;
}
// the start's first bytes as the game file has them (or not)
static uint8_t code[16];
const void *prepatch_original_code(const void *address, size_t size) {
    (void)address, (void)size;
    return code;
}

int main(void) {
    static const uint8_t start_bytes[] = { 0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xFE };
    memcpy(code, start_bytes, sizeof(start_bytes));
    uint64_t base = 0x100000000ULL;
    CHECK(voice_mic_allowed(1) && !voice_mic_allowed(0));
    // 1, the default: nothing hooked, the game's own path
    voice_mic_init(base, 1);
    CHECK(hooks == 0);
    // 0 with other code than expected there: nothing hooked
    code[9] = 0x00;
    voice_mic_init(base, 0);
    CHECK(hooks == 0);
    code[9] = 0xFE;
    // 0: the start is hooked, at its address
    voice_mic_init(base, 0);
    CHECK(hooks == 1 && hooked_at == (void *)(uintptr_t)(base + ADDR_Voice_StartRecording) &&
          hooked_with == (void *)fake_Voice_StartRecording);
    // the voice frame's start: refused with the layer's own failure, Steam never asked, said once
    static uint8_t capture[64];
    CHECK(fake_Voice_StartRecording(capture) == -1 && steam_starts == 0 && said);
    CHECK(fake_Voice_StartRecording(capture) == -1 && steam_starts == 0);
    if (failures == 0)
        printf("test_voice_mic: all passed\n");
    return failures != 0;
}
