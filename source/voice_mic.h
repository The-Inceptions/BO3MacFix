// VoiceMicrophone: whether the game has Steam record the microphone for voice chat, see voice_mic.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// 1 (the default): as the game always did, the microphone is recorded from the voice system's first frame to the end;
// 0: the voice system's start of recording is refused, so the game never asks Steam to record
void voice_mic_init(uint64_t game_base_address, int setting);

// whether the voice system may start recording, for the setting (the rule, for the tests)
bool voice_mic_allowed(int setting);
