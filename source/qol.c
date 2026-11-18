// Quality of life fixes that only replace system calls the game makes (DYLD_INTERPOSE), so they don't touch the
// game's code.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <os/lock.h>
#include <mach-o/dyld.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include "display_follow.h"
#include "dyld-interposing.h"
#include "gc_bridge.h"
#include "prepatch.h"
#include "qol.h"
#include "script_errors.h"

static bool exit_fix = false;
static atomic_bool audio_follow = false;

#pragma mark - quitting without a crash

// Quitting crashes every time: the game thread calls exit(), whose static destructors destroy the mutex Aspyr's
// main thread waits on for tasks, so the main thread's next lock throws std::system_error and abort()s. The
// config, stats and loadouts are all saved before that exit() (Com_Quit saves the config, then Sys_Quit exits), so
// when exit() comes from any thread but the main one, flush what's buffered and end the process without the
// destructors, like Aspyr's own exit path on the main thread can. Nothing registered with atexit runs then either,
// so save what display_follow remembers about each display first, and turn the DualSense's adaptive triggers off.
__attribute__((noreturn)) void exit_new(int status) {
    if (exit_fix && !pthread_main_np()) {
        printf("BO3MacFix: the game quit with status %i, ending it without running static destructors\n", status);
        prepatch_report_at_quit();
        script_errors_quit();
        display_follow_save();
        gc_bridge_quit();
        fflush(NULL);
        _exit(status);
    }
    exit(status);
}
DYLD_INTERPOSE(exit_new, exit);

#pragma mark - audio following the default output device

// The game's SDL 2.0.7 pins its audio queue to the output device that was the default when it started, and when
// that device goes away (AirPods disconnecting) it treats audio as unplugged and plays nothing. Instead, move the
// game's audio queues to whatever becomes the default output, and keep SDL from giving up on a device that died.
//
// Where a move runs: SDL makes its queue on an audio thread of its own (0x100142270), on that thread's run loop, sets
// it up there (buffers, start) and then turns that run loop in 0.1 s slices until it closes the device. Its close
// (0x100141f50) disposes the queue first (0x100141fa4), while that thread still turns its run loop, and only then
// stops and joins the thread. The device can only change while the queue is stopped, and AudioQueueStop with
// "immediate" waits for the thread the queue's callbacks come on to turn its run loop: asked from another thread that
// is up to about 15 s (CoreAudio's own limit) when that thread doesn't turn it (measured on this macOS, arm64 and
// x86_64: 14.7 s, against 0.01 s while it turns). The default output's listener runs on another thread, and it used to
// stop the queues there, holding queues_lock, which SDL's thread takes while it sets a queue up: a device change while
// the game opened its audio (the opening waits for SDL's set-up) froze both for those 15 s. (At the close the thread
// still turns, so a stop came back at once there; only after an open that failed, whose thread had ended before the
// close, did it wait too.) Done on the queue's own thread, the stop waits for nothing (measured too). So the listener
// only hands each queue's move to the run loop the queue was made with, where it runs the next time that thread turns
// it, and never once the thread has ended. A dispose takes the queue out of the table first: a move still waiting then
// finds nothing, and one already running holds queues_lock, so the close waits for its end, at most about half a
// second. queues_lock is only taken there and for the table's short changes, never while waiting on CoreAudio for
// another thread.
//
// A move right after the queue was started (a second change in a row, or one right after SDL's set-up) spends about
// half a second in the stop (0.52 s, measured on both CPUs): that much without sound then. One while it plays takes
// about 0.02 s.
#define Max_Queues 8
#define Max_Buffers 16
typedef struct {
    AudioQueueRef queue;        // NULL for a free place
    CFRunLoopRef run_loop;      // the run loop its callbacks come on (retained)
    CFStringRef run_loop_mode;  // and their mode (retained)
    AudioQueueBufferRef buffers[Max_Buffers];
} queue_entry_t;
static queue_entry_t queues[Max_Queues];
static os_unfair_lock queues_lock = OS_UNFAIR_LOCK_INIT;

static CFStringRef copy_default_output_uid(void) {
    AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                           0 /* main element */ };
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) != noErr ||
        device == kAudioObjectUnknown)
        return NULL;
    CFStringRef uid = NULL;
    address.mSelector = kAudioDevicePropertyDeviceUID;
    size = sizeof(uid);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &uid) != noErr)
        return NULL;
    return uid;
}

// what a move calls: CoreAudio's own (the tests put stand-ins here)
static OSStatus (*queue_stop)(AudioQueueRef queue, Boolean immediate) = AudioQueueStop;
static OSStatus (*queue_set_property)(AudioQueueRef queue, AudioQueuePropertyID id, const void *data,
                                      UInt32 size) = AudioQueueSetProperty;
static OSStatus (*queue_enqueue)(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 descriptions,
                                 const AudioStreamPacketDescription *description) = AudioQueueEnqueueBuffer;
static OSStatus (*queue_start)(AudioQueueRef queue, const AudioTimeStamp *start) = AudioQueueStart;
static CFStringRef (*copy_default_uid)(void) = copy_default_output_uid;

// one queue to the default output, on the thread its callbacks come on; nothing if it was disposed meanwhile
static void move_queue(AudioQueueRef queue) {
    CFStringRef uid = copy_default_uid();
    if (uid == NULL)
        return;
    char name[256] = "?";
    CFStringGetCString(uid, name, sizeof(name), kCFStringEncodingUTF8);
    os_unfair_lock_lock(&queues_lock);
    for (int i = 0; i < Max_Queues; i++) {
        queue_entry_t *entry = &queues[i];
        if (entry->queue != queue)
            continue;
        // the device can only change while the queue is stopped. Stopping hands its buffers back to SDL's
        // callback, which queues them again once it's started.
        OSStatus status = queue_stop(queue, true);
        if (status == noErr)
            status = queue_set_property(queue, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
        // changing the device drops the buffers SDL had queued, so queue them again (silent) to get its callback
        // going; ones still queued just fail
        for (int b = 0; b < Max_Buffers && entry->buffers[b] != NULL; b++) {
            AudioQueueBufferRef buffer = entry->buffers[b];
            memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
            buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
            queue_enqueue(queue, buffer, 0, NULL);
        }
        OSStatus started = queue_start(queue, NULL);
        printf("BO3MacFix: moved the game's audio to the default output %s (%d, %d)\n", name, (int)status, (int)started);
    }
    os_unfair_lock_unlock(&queues_lock);
    CFRelease(uid);
}

// each queue's move handed to the run loop its callbacks come on (see above): this thread waits for none of them
static void move_queues_to_default_output(void) {
    os_unfair_lock_lock(&queues_lock);
    for (int i = 0; i < Max_Queues; i++) {
        AudioQueueRef queue = queues[i].queue;
        if (queue == NULL)
            continue;
        CFRunLoopPerformBlock(queues[i].run_loop, queues[i].run_loop_mode, ^{
            move_queue(queue);
        });
        CFRunLoopWakeUp(queues[i].run_loop);
    }
    os_unfair_lock_unlock(&queues_lock);
}

static OSStatus default_output_changed(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses,
                                       void *context) {
    if (atomic_load(&audio_follow))
        move_queues_to_default_output();
    return noErr;
}

// A queue made with a run loop for its callbacks is kept, with that run loop. One whose callbacks come on CoreAudio's
// own thread (none of the game's) isn't: its move would have no thread of its own to run on.
static void remember_queue(AudioQueueRef queue, CFRunLoopRef run_loop, CFStringRef run_loop_mode) {
    if (run_loop == NULL)
        return;
    os_unfair_lock_lock(&queues_lock);
    for (int i = 0; i < Max_Queues; i++) {
        if (queues[i].queue == NULL) {
            memset(&queues[i], 0, sizeof(queues[i]));
            queues[i].queue = queue;
            queues[i].run_loop = (CFRunLoopRef)CFRetain(run_loop);
            // (no mode is the common modes, as AudioQueueNewOutput takes it)
            queues[i].run_loop_mode = (CFStringRef)CFRetain(run_loop_mode != NULL ? run_loop_mode
                                                                                  : kCFRunLoopCommonModes);
            break;
        }
    }
    os_unfair_lock_unlock(&queues_lock);
}

static void remember_buffer(AudioQueueRef queue, AudioQueueBufferRef buffer) {
    os_unfair_lock_lock(&queues_lock);
    for (int i = 0; i < Max_Queues; i++) {
        if (queues[i].queue != queue)
            continue;
        for (int b = 0; b < Max_Buffers; b++) {
            if (queues[i].buffers[b] == NULL) {
                queues[i].buffers[b] = buffer;
                break;
            }
        }
    }
    os_unfair_lock_unlock(&queues_lock);
}

// the queue goes: out of the table first, so that a move still waiting for its run loop finds nothing
static void forget_queue(AudioQueueRef queue) {
    CFRunLoopRef run_loops[Max_Queues];
    CFStringRef modes[Max_Queues];
    int forgotten = 0;
    os_unfair_lock_lock(&queues_lock);
    for (int i = 0; i < Max_Queues; i++) {
        if (queues[i].queue == queue) {
            run_loops[forgotten] = queues[i].run_loop;
            modes[forgotten++] = queues[i].run_loop_mode;
            memset(&queues[i], 0, sizeof(queues[i]));
        }
    }
    os_unfair_lock_unlock(&queues_lock);
    for (int i = 0; i < forgotten; i++) {
        CFRelease(run_loops[i]);
        CFRelease(modes[i]);
    }
}

OSStatus AudioQueueNewOutput_new(const AudioStreamBasicDescription *format, AudioQueueOutputCallback callback,
                                 void *user_data, CFRunLoopRef run_loop, CFStringRef run_loop_mode, UInt32 flags,
                                 AudioQueueRef *out_queue) {
    OSStatus status = AudioQueueNewOutput(format, callback, user_data, run_loop, run_loop_mode, flags, out_queue);
    if (status == noErr && atomic_load(&audio_follow))
        remember_queue(*out_queue, run_loop, run_loop_mode);
    return status;
}
DYLD_INTERPOSE(AudioQueueNewOutput_new, AudioQueueNewOutput);

OSStatus AudioQueueDispose_new(AudioQueueRef queue, Boolean immediate) {
    forget_queue(queue);
    return AudioQueueDispose(queue, immediate);
}
DYLD_INTERPOSE(AudioQueueDispose_new, AudioQueueDispose);

OSStatus AudioQueueAllocateBuffer_new(AudioQueueRef queue, UInt32 size, AudioQueueBufferRef *out_buffer) {
    OSStatus status = AudioQueueAllocateBuffer(queue, size, out_buffer);
    if (status == noErr && atomic_load(&audio_follow))
        remember_buffer(queue, *out_buffer);
    return status;
}
DYLD_INTERPOSE(AudioQueueAllocateBuffer_new, AudioQueueAllocateBuffer);

// whether the code that called us is the game's (its SDL), not a system framework the game uses
static bool called_by_game(const void *return_address) {
    Dl_info info;
    return dladdr(return_address, &info) != 0 && info.dli_fbase == (const void *)_dyld_get_image_header(0);
}

// SDL listens for its device dying to mark audio as unplugged for good, which we don't want any more
OSStatus AudioObjectAddPropertyListener_new(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                            AudioObjectPropertyListenerProc listener, void *client_data) {
    if (atomic_load(&audio_follow) && address != NULL && address->mSelector == kAudioDevicePropertyDeviceIsAlive &&
        called_by_game(__builtin_return_address(0)))
        return noErr;
    return AudioObjectAddPropertyListener(object, address, listener, client_data);
}
DYLD_INTERPOSE(AudioObjectAddPropertyListener_new, AudioObjectAddPropertyListener);

OSStatus AudioObjectRemovePropertyListener_new(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                               AudioObjectPropertyListenerProc listener, void *client_data) {
    if (atomic_load(&audio_follow) && address != NULL && address->mSelector == kAudioDevicePropertyDeviceIsAlive &&
        called_by_game(__builtin_return_address(0)))
        return noErr;
    return AudioObjectRemovePropertyListener(object, address, listener, client_data);
}
DYLD_INTERPOSE(AudioObjectRemovePropertyListener_new, AudioObjectRemovePropertyListener);

void qol_init(int exit_fix_setting, int audio_follow_setting) {
    const char *env = getenv("BO3MACFIX_EXITFIX");
    exit_fix = (env != NULL ? atoi(env) : exit_fix_setting) != 0;
    env = getenv("BO3MACFIX_AUDIOFOLLOWSDEFAULT");
    bool follow = (env != NULL ? atoi(env) : audio_follow_setting) != 0;
    if (exit_fix)
        printf("BO3MacFix: quitting won't crash (ExitFix)\n");
    if (follow) {
        AudioObjectPropertyAddress address = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                               0 /* main element */ };
        // our own listener, registered directly (calls from our image aren't interposed)
        if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &address, default_output_changed, NULL) == noErr) {
            atomic_store(&audio_follow, true);
            printf("BO3MacFix: game audio follows the default output device (AudioFollowsDefault)\n");
        }
    }
}

void qol_set_exit_fix(int on) {
    exit_fix = on != 0;
}
