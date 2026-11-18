// VoiceMicrophone: the game has Steam record the microphone for its whole session, whether or not anybody can hear the
// player. 0 keeps the microphone closed.
//
// The engine's voice system (CoD's "winvoice": its init 0x100e1431a registers winvoice_mic_mute, winvoice_mic_reclevel,
// winvoice_save_voice and winvoice_mic_scaler) starts recording at its first frame (0x100e14b85: when its capture layer
// and its voice output came up, which on the Mac they always do) and stops only when it shuts down. Nothing gates it,
// not a dvar, not being offline or alone. Aspyr's capture layer records through Steam: its start (0x100fb7cc6) calls
// SteamUser()->StartVoiceRecording() (ISteamUser019's eighth virtual), and Steam opens the Mac's microphone in its own
// process for as long as that lasts: macOS shows the microphone in use for the whole game (coreaudiod's
// "BuiltInMicrophoneDevice" assertion, created for steam_osx about 16 s after the game starts), and Steam's capture runs
// (measured 2026-11-18 from outside: steam_osx 0.042 cores and 410 wakeups a second with the game, 0.025 and 166
// without; wip/night/perf-battery2.md).
//
// With VoiceMicrophone = 0 the start is refused: the hook returns the layer's own failure (-1) without calling Steam.
// The voice frame doesn't look at the result (it marks recording started and never tries again), so nothing is
// recorded or sent; the voice frame's later reads find nothing, as on a Mac without a microphone, and other players
// are still heard. With 1 (the default) nothing is hooked: the game's own path.
#include <stdio.h>
#include <string.h>
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"
#include "voice_mic.h"

bool voice_mic_allowed(int setting) {
    return setting != 0;
}

static bool said = false;

// the capture layer's start: int (void *capture), 0 started, -1 not
install_hook_name(Voice_StartRecording, int, void *capture) {
    if (!said) {
        said = true;
        printf("voice: the game asked to record the microphone; it stays closed (VoiceMicrophone 0)\n");
    }
    return -1;
}

void voice_mic_init(uint64_t game_base_address, int setting) {
    if (voice_mic_allowed(setting))
        return;
    // push rbp; mov rbp, rsp; push r14; push rbx; mov r14, rdi
    static const uint8_t expected[] = { 0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xFE };
    void *start = (void *)(game_base_address + ADDR_Voice_StartRecording);
    if (memcmp(prepatch_original_code(start, sizeof(expected)), expected, sizeof(expected)) != 0) {
        printf("unexpected code at %08x, VoiceMicrophone 0 can't keep the microphone closed\n", ADDR_Voice_StartRecording);
        return;
    }
    install_hook_Voice_StartRecording(start);
    printf("voice: the microphone stays closed (VoiceMicrophone 0): no voice chat from this Mac, others are still heard\n");
}
