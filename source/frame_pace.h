// How evenly frames reach the screen, for FrameStats' "frame pacing:" line (BO3MACFIX_FRAMEPACESTATS=1): from the
// times the frames were actually shown (MTLDrawable.presentedTime, gpu_diag.m), how many refreshes of the display
// each stayed on screen. The frame stats line times the render thread's presents, which can't tell 86 fps of evenly
// spaced frames from 86 fps of frames alternating between one and two refreshes; this can. Plain C, no game or
// macOS calls, so tests/test_frame_pace.c tests it as is.
#pragma once

// frames that stayed 1, 2, 3, and 4 or more refreshes on screen
enum { Frame_Pace_Buckets = 4 };

typedef struct {
    int frames;                            // frames whose time on screen is known: one less than the times shown
    int refreshes[Frame_Pace_Buckets];     // of those, how many stayed 1, 2, 3, and 4 or more refreshes
    int changes;                           // frames that stayed another number of refreshes than the one before
    int off_grid;                          // frames that stayed a quarter refresh or more off a whole number of them
    double fps;                            // frames shown per second over the time the times span
    double low_1_fps;                      // the 1% low: the frame rate of the slowest 1% of the frames
    double worst_ms;                       // the longest a frame stayed on screen
} frame_pace_t;

// times: when each frame was shown, in seconds, as they were recorded (sorted here; times <= 0, frames never shown,
// are left out). refresh: the display's shortest refresh interval in seconds. work: room for count doubles.
void frame_pace_summarize(const double *times, int count, double refresh, double *work, frame_pace_t *out);
