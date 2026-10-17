// Quality of life fixes that only replace system calls the game makes, see qol.c.
#pragma once

// exit_fix: quit without Aspyr's shutdown crash. audio_follow: game audio follows the default output device.
// Each can be overridden at env (BO3MACFIX_EXITFIX, BO3MACFIX_AUDIOFOLLOWSDEFAULT).
void qol_init(int exit_fix, int audio_follow);
