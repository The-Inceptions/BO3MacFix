// Frame queue, pacing and upscaling tweaks for Aspyr's Metal presenter (its D3D Present is at 0x1000e8b78).
// - FramesInFlight: Aspyr creates its frames-in-flight semaphore with a fixed count of 2, and its
//   IDXGIDevice1::SetMaximumFrameLatency ignores the value, so MaxFrameLatency in config.ini does nothing.
// - FramePacingFPS: presents with presentDrawable:afterMinimumDuration: instead of presentDrawable:, so
//   each frame stays on screen for at least 1/fps seconds and frames come out evenly on ProMotion displays.
// - MetalFXUpscale: when the game runs below the screen's resolution, upscales each frame to the screen's
//   native size with MetalFX instead of letting Core Animation stretch it, see metalfx.m.
// - FrameStats: logs the frame rate, 1% low and worst frame time every few seconds, for benchmarking.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <objc/runtime.h>
#include <objc/message.h>

#include "gpu_tweaks.h"
#include "metalfx.h"
#include "prepatch.h"
#include "offsets.h"

extern uint64_t game_base_address;

static double present_min_duration; // 0 when not pacing
static SEL sel_present_after_minimum_duration;
static int metalfx_active = 0;
static int frame_stats_seconds = 0; // 0 when not logging frame stats

static int compare_floats(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// times each present, and every frame_stats_seconds logs the average frame rate, the 1% low (the frame rate of the
// slowest 1% of frames) and the worst frame time
static void frame_stats_tick(void) {
    static mach_timebase_info_data_t timebase;
    static uint64_t last_present = 0, window_start = 0;
    static float frame_ms[8192];
    static int frames = 0;
    if (timebase.denom == 0)
        mach_timebase_info(&timebase);
    uint64_t now = mach_absolute_time();
    if (last_present != 0 && frames < (int)(sizeof(frame_ms) / sizeof(frame_ms[0])))
        frame_ms[frames++] = (float)((double)(now - last_present) * timebase.numer / timebase.denom / 1e6);
    last_present = now;
    if (window_start == 0)
        window_start = now;
    double seconds = (double)(now - window_start) * timebase.numer / timebase.denom / 1e9;
    if (seconds < frame_stats_seconds || frames == 0)
        return;
    qsort(frame_ms, frames, sizeof(frame_ms[0]), compare_floats);
    float low_1 = frame_ms[(int)(frames * 0.99)];
    printf("frame stats: %.1f fps, 1%% low %.1f fps, worst frame %.1f ms (%i frames in %.1f s)\n",
           frames / seconds, 1000.0f / low_1, frame_ms[frames - 1], frames, seconds);
    frames = 0;
    window_start = now;
}

// called instead of objc_msgSend for Aspyr's [commandBuffer presentDrawable:drawable]
static void present_drawable(id command_buffer, SEL sel, id drawable) {
    if (frame_stats_seconds > 0)
        frame_stats_tick();
    if (metalfx_active)
        metalfx_before_present(command_buffer);
    static int supported = -1;
    if (present_min_duration > 0 && supported == -1)
        supported = class_respondsToSelector(object_getClass(command_buffer), sel_present_after_minimum_duration);
    if (present_min_duration > 0 && supported)
        ((void (*)(id, SEL, id, double))objc_msgSend)(command_buffer, sel_present_after_minimum_duration, drawable, present_min_duration);
    else
        ((void (*)(id, SEL, id))objc_msgSend)(command_buffer, sel, drawable);
}

// a pointer-sized slot that a rip-relative call at address can reach (+-2GB), on a page past the end of the game's
// image. The calls we redirect are all in Aspyr's present function, so they share one page.
static void **alloc_slot_near(uint64_t address) {
    static void **page = NULL;
    static unsigned int used = 0;
    for (uint64_t at = (address + 0x20000000) & ~0xFFFFFULL; page == NULL && at < address + 0x78000000; at += 0x1000000) {
        mach_vm_address_t slot = at;
        if (mach_vm_allocate(mach_task_self(), &slot, 0x1000, VM_FLAGS_FIXED) == KERN_SUCCESS)
            page = (void **)slot;
    }
    if (page == NULL || used >= 0x1000 / sizeof(void *) || (uint64_t)page - address >= 0x7FF00000)
        return NULL;
    return &page[used++];
}

// points a call qword ptr [rip + disp32] that goes through objc_msgSend's pointer at function instead,
// returns 1 if the game's code was changed
static int redirect_objc_msgSend_call(uint32_t address, const uint8_t *expected, void *function, code_patch_fn checked_code_patch) {
    // the pre-patched executable already calls a stub here that jumps through a slot
    if (prepatch_redirect_call(address, function))
        return 1;
    uint8_t *code = (uint8_t *)(game_base_address + address);
    if (memcmp(code, expected, 6) != 0) {
        printf("unexpected code at %08x, not patching\n", address);
        return 0;
    }
    void **slot = alloc_slot_near((uint64_t)code);
    if (slot == NULL) {
        printf("no free memory near %08x, not patching\n", address);
        return 0;
    }
    *slot = function;
    int32_t disp = (int32_t)((int64_t)(uint64_t)slot - (int64_t)((uint64_t)code + 6));
    uint8_t call_slot[6] = { 0xFF, 0x15 };
    memcpy(call_slot + 2, &disp, sizeof(disp));
    checked_code_patch(address, expected, call_slot, sizeof(call_slot));
    return memcmp(code, call_slot, sizeof(call_slot)) == 0;
}

// pacing and MetalFX both need Aspyr's presentDrawable: call to go through present_drawable
static int redirect_present(code_patch_fn checked_code_patch) {
    static int redirected = -1;
    static const uint8_t call_objc_msgSend[] = { 0xFF, 0x15, 0xD9, 0xDB, 0x87, 0x01 };
    if (redirected == -1)
        redirected = redirect_objc_msgSend_call(ADDR_ASL_Present_PresentDrawableCall, call_objc_msgSend,
                                                (void *)present_drawable, checked_code_patch);
    return redirected;
}

static void frames_in_flight_patch(int frames, code_patch_fn checked_code_patch) {
    static const uint8_t mov_edi_2[] = { 0xBF, 0x02, 0x00, 0x00, 0x00 };
    uint8_t mov_edi_frames[] = { 0xBF, (uint8_t)frames, 0x00, 0x00, 0x00 };
    checked_code_patch(ADDR_ASL_FramesInFlightSemaphoreInstr, mov_edi_2, mov_edi_frames, sizeof(mov_edi_frames));
    printf("allowing %i frames in flight\n", frames);
}

static void frame_pacing_patch(int fps, code_patch_fn checked_code_patch) {
    if (!redirect_present(checked_code_patch)) {
        printf("not pacing frames\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    present_min_duration = 1.0 / fps;
    printf("pacing frames at %i fps\n", fps);
}

void gpu_tweaks_init(int frames_in_flight, int pacing_fps, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_FRAMESINFLIGHT");
    if (env != NULL)
        frames_in_flight = atoi(env);
    env = getenv("BO3MACFIX_FRAMEPACINGFPS");
    if (env != NULL)
        pacing_fps = atoi(env);

    // the ring of command buffers behind it has 3 slots, so more than 3 can't be in flight anyway
    if (frames_in_flight == 1 || frames_in_flight == 3)
        frames_in_flight_patch(frames_in_flight, checked_code_patch);
    if (pacing_fps >= 30 && pacing_fps <= 240)
        frame_pacing_patch(pacing_fps, checked_code_patch);
}

void gpu_tweaks_metalfx_init(int metalfx_upscale, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_METALFXUPSCALE");
    if (env != NULL)
        metalfx_upscale = atoi(env);
    if (metalfx_upscale == 0)
        return;

    // Aspyr's [blitEncoder copyFromTexture:backbuffer ... toTexture:drawable.texture ...] in its present function
    static const uint8_t call_objc_msgSend[] = { 0xFF, 0x15, 0xDC, 0xDD, 0x87, 0x01 };
    if (!redirect_present(checked_code_patch) ||
        !redirect_objc_msgSend_call(ADDR_ASL_Present_CopyToDrawableCall, call_objc_msgSend, metalfx_copy_function(),
                                    checked_code_patch)) {
        printf("MetalFX: couldn't hook Aspyr's present, not upscaling\n");
        return;
    }
    metalfx_active = metalfx_init();
}

void gpu_tweaks_frame_stats_init(int seconds, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_FRAMESTATS");
    if (env != NULL)
        seconds = atoi(env);
    if (seconds <= 0)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no frame stats\n");
        return;
    }
    frame_stats_seconds = seconds;
    printf("logging frame stats every %i s\n", seconds);
}
