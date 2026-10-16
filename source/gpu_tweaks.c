// Frame queue and pacing tweaks for Aspyr's Metal presenter (its D3D Present is at 0x1000e8b78).
// - FramesInFlight: Aspyr creates its frames-in-flight semaphore with a fixed count of 2, and its
//   IDXGIDevice1::SetMaximumFrameLatency ignores the value, so MaxFrameLatency in config.ini does nothing.
// - FramePacingFPS: presents with presentDrawable:afterMinimumDuration: instead of presentDrawable:, so
//   each frame stays on screen for at least 1/fps seconds and frames come out evenly on ProMotion displays.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <objc/runtime.h>
#include <objc/message.h>

#include "gpu_tweaks.h"
#include "offsets.h"

extern uint64_t game_base_address;

static double present_min_duration;
static SEL sel_present_after_minimum_duration;

// called instead of objc_msgSend for Aspyr's [commandBuffer presentDrawable:drawable]
static void present_drawable_paced(id command_buffer, SEL sel, id drawable) {
    static int supported = -1;
    if (supported == -1)
        supported = class_respondsToSelector(object_getClass(command_buffer), sel_present_after_minimum_duration);
    if (supported)
        ((void (*)(id, SEL, id, double))objc_msgSend)(command_buffer, sel_present_after_minimum_duration, drawable, present_min_duration);
    else
        ((void (*)(id, SEL, id))objc_msgSend)(command_buffer, sel, drawable);
}

// a pointer-sized slot that a rip-relative call at address can reach (+-2GB), placed past the end of the game's image
static void **alloc_slot_near(uint64_t address) {
    for (uint64_t page = (address + 0x20000000) & ~0xFFFFFULL; page < address + 0x78000000; page += 0x1000000) {
        mach_vm_address_t slot = page;
        if (mach_vm_allocate(mach_task_self(), &slot, 0x1000, VM_FLAGS_FIXED) == KERN_SUCCESS)
            return (void **)slot;
    }
    return NULL;
}

static void frames_in_flight_patch(int frames, code_patch_fn checked_code_patch) {
    static const uint8_t mov_edi_2[] = { 0xBF, 0x02, 0x00, 0x00, 0x00 };
    uint8_t mov_edi_frames[] = { 0xBF, (uint8_t)frames, 0x00, 0x00, 0x00 };
    checked_code_patch(ADDR_ASL_FramesInFlightSemaphoreInstr, mov_edi_2, mov_edi_frames, sizeof(mov_edi_frames));
    printf("allowing %i frames in flight\n", frames);
}

static void frame_pacing_patch(int fps, code_patch_fn checked_code_patch) {
    uint64_t call_address = game_base_address + ADDR_ASL_Present_PresentDrawableCall;
    void **slot = alloc_slot_near(call_address);
    if (slot == NULL) {
        printf("no free memory near the present call, not pacing frames\n");
        return;
    }
    present_min_duration = 1.0 / fps;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    *slot = (void *)present_drawable_paced;

    // call qword ptr [rip + disp32] goes through objc_msgSend's pointer, point it at our slot instead
    static const uint8_t call_objc_msgSend[] = { 0xFF, 0x15, 0xD9, 0xDB, 0x87, 0x01 };
    int32_t disp = (int32_t)((int64_t)(uint64_t)slot - (int64_t)(call_address + sizeof(call_objc_msgSend)));
    uint8_t call_slot[6] = { 0xFF, 0x15 };
    memcpy(call_slot + 2, &disp, sizeof(disp));
    checked_code_patch(ADDR_ASL_Present_PresentDrawableCall, call_objc_msgSend, call_slot, sizeof(call_slot));
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
