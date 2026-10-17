// Quality of life fixes that only replace system calls the game makes (DYLD_INTERPOSE), so they don't touch the
// game's code.
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <os/lock.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include "dyld-interposing.h"
#include "qol.h"

static bool exit_fix = false;
static atomic_bool audio_follow = false;

#pragma mark - quitting without a crash

// Quitting crashes every time: the game thread calls exit(), whose static destructors destroy the mutex Aspyr's
// main thread waits on for tasks, so the main thread's next lock throws std::system_error and abort()s. The
// config, stats and loadouts are all saved before that exit() (Com_Quit saves the config, then Sys_Quit exits), so
// when exit() comes from any thread but the main one, flush what's buffered and end the process without the
// destructors, like Aspyr's own exit path on the main thread can.
__attribute__((noreturn)) void exit_new(int status) {
    if (exit_fix && !pthread_main_np()) {
        printf("BO3MacFix: the game quit with status %i, ending it without running static destructors\n", status);
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
#define Max_Queues 8
#define Max_Buffers 16
static AudioQueueRef queues[Max_Queues];
static AudioQueueBufferRef buffers[Max_Queues][Max_Buffers];
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

static void move_queues_to_default_output(void) {
    CFStringRef uid = copy_default_output_uid();
    if (uid == NULL)
        return;
    char name[256] = "?";
    CFStringGetCString(uid, name, sizeof(name), kCFStringEncodingUTF8);
    os_unfair_lock_lock(&queues_lock);
    for (int i = 0; i < Max_Queues; i++) {
        if (queues[i] == NULL)
            continue;
        // the device can only change while the queue is stopped. Stopping hands its buffers back to SDL's
        // callback, which queues them again once it's started.
        OSStatus status = AudioQueueStop(queues[i], true);
        if (status == noErr)
            status = AudioQueueSetProperty(queues[i], kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
        // changing the device drops the buffers SDL had queued, so queue them again (silent) to get its callback
        // going; ones still queued just fail
        for (int b = 0; b < Max_Buffers && buffers[i][b] != NULL; b++) {
            memset(buffers[i][b]->mAudioData, 0, buffers[i][b]->mAudioDataBytesCapacity);
            buffers[i][b]->mAudioDataByteSize = buffers[i][b]->mAudioDataBytesCapacity;
            AudioQueueEnqueueBuffer(queues[i], buffers[i][b], 0, NULL);
        }
        OSStatus started = AudioQueueStart(queues[i], NULL);
        printf("BO3MacFix: moved the game's audio to the default output %s (%d, %d)\n", name, (int)status, (int)started);
    }
    os_unfair_lock_unlock(&queues_lock);
    CFRelease(uid);
}

static OSStatus default_output_changed(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses,
                                       void *context) {
    if (atomic_load(&audio_follow))
        move_queues_to_default_output();
    return noErr;
}

OSStatus AudioQueueNewOutput_new(const AudioStreamBasicDescription *format, AudioQueueOutputCallback callback,
                                 void *user_data, CFRunLoopRef run_loop, CFStringRef run_loop_mode, UInt32 flags,
                                 AudioQueueRef *out_queue) {
    OSStatus status = AudioQueueNewOutput(format, callback, user_data, run_loop, run_loop_mode, flags, out_queue);
    if (status == noErr && atomic_load(&audio_follow)) {
        os_unfair_lock_lock(&queues_lock);
        for (int i = 0; i < Max_Queues; i++) {
            if (queues[i] == NULL) {
                queues[i] = *out_queue;
                break;
            }
        }
        os_unfair_lock_unlock(&queues_lock);
    }
    return status;
}
DYLD_INTERPOSE(AudioQueueNewOutput_new, AudioQueueNewOutput);

OSStatus AudioQueueDispose_new(AudioQueueRef queue, Boolean immediate) {
    os_unfair_lock_lock(&queues_lock);
    for (int i = 0; i < Max_Queues; i++) {
        if (queues[i] == queue) {
            queues[i] = NULL;
            memset(buffers[i], 0, sizeof(buffers[i]));
        }
    }
    os_unfair_lock_unlock(&queues_lock);
    return AudioQueueDispose(queue, immediate);
}
DYLD_INTERPOSE(AudioQueueDispose_new, AudioQueueDispose);

OSStatus AudioQueueAllocateBuffer_new(AudioQueueRef queue, UInt32 size, AudioQueueBufferRef *out_buffer) {
    OSStatus status = AudioQueueAllocateBuffer(queue, size, out_buffer);
    if (status == noErr && atomic_load(&audio_follow)) {
        os_unfair_lock_lock(&queues_lock);
        for (int i = 0; i < Max_Queues; i++) {
            if (queues[i] != queue)
                continue;
            for (int b = 0; b < Max_Buffers; b++) {
                if (buffers[i][b] == NULL) {
                    buffers[i][b] = *out_buffer;
                    break;
                }
            }
        }
        os_unfair_lock_unlock(&queues_lock);
    }
    return status;
}
DYLD_INTERPOSE(AudioQueueAllocateBuffer_new, AudioQueueAllocateBuffer);

// SDL listens for its device dying to mark audio as unplugged for good, which we don't want any more
OSStatus AudioObjectAddPropertyListener_new(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                            AudioObjectPropertyListenerProc listener, void *client_data) {
    if (atomic_load(&audio_follow) && address != NULL && address->mSelector == kAudioDevicePropertyDeviceIsAlive)
        return noErr;
    return AudioObjectAddPropertyListener(object, address, listener, client_data);
}
DYLD_INTERPOSE(AudioObjectAddPropertyListener_new, AudioObjectAddPropertyListener);

OSStatus AudioObjectRemovePropertyListener_new(AudioObjectID object, const AudioObjectPropertyAddress *address,
                                               AudioObjectPropertyListenerProc listener, void *client_data) {
    if (atomic_load(&audio_follow) && address != NULL && address->mSelector == kAudioDevicePropertyDeviceIsAlive)
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
