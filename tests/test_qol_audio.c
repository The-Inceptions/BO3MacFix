// qol.c's moves of the game's audio queues to a new default output (AudioFollowsDefault), against stand-ins for
// CoreAudio that behave as the real one was measured to on this macOS (both CPUs' builds): AudioQueueStop(queue,
// true) asked from another thread waits for the thread the queue's callbacks come on to turn its run loop (CoreAudio
// gives up after about 15 s; the stand-in after 2 s), and asked on that thread itself waits for nothing. A made-up SDL
// audio thread sets its queue up as the game's SDL 2.0.7 does (0x100142270): the queue made on its own run loop, its
// buffers allocated (each through the wrappers' table, under queues_lock), then its run loop turned in 0.1 s slices
// until it closes. The game's close (0x100141f50) disposes the queue while that thread still turns its run loop and
// only then ends the thread; an open that fails ends its thread before the close. A stop right after the queue was
// started takes about half a second on the queue's own thread (measured: 0.52 s); the stand-in can take a set time.
// No sound is made: nothing of CoreAudio's is called.
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// what qol.c's quitting part asks of the rest of BO3MacFix, never called here
void prepatch_report_at_quit(void) {
}
void script_errors_quit(void) {
}
void display_follow_save(void) {
}
void gc_bridge_quit(void) {
}

#include "../source/qol.c"

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// ---- CoreAudio's stand-ins

static int fake_queue; // an AudioQueueRef stand-in: the code only passes it on
#define Queue ((AudioQueueRef)&fake_queue)
static CFRunLoopRef queue_run_loop; // the run loop the made-up audio thread made the queue on
static atomic_int stops, stops_elsewhere, set_devices, enqueued, starts;
static atomic_int stop_ms_on_own_thread; // how long a stop on the queue's own thread takes (0.52 s after a start)
static atomic_int stop_began;             // a stop has begun on the queue's own thread
static atomic_long longest_stop_ms;
static atomic_int listener_asked; // a stop came from another thread: the made-up SDL thread goes on with its set-up

// whether the run loop runs a block within that time (the flag is left to the block when it runs late)
static bool turns_within(CFRunLoopRef loop, double seconds) {
    atomic_int *ran = calloc(1, sizeof(*ran));
    CFRunLoopPerformBlock(loop, kCFRunLoopDefaultMode, ^{
        atomic_store(ran, 1);
    });
    CFRunLoopWakeUp(loop);
    double until = now() + seconds;
    while (!atomic_load(ran) && now() < until)
        usleep(1000);
    return atomic_load(ran) != 0;
}

static OSStatus fake_stop(AudioQueueRef queue, Boolean immediate) {
    CHECK(queue == Queue && immediate);
    atomic_fetch_add(&stops, 1);
    if (CFRunLoopGetCurrent() == queue_run_loop) {
        // on the queue's own thread: nothing to wait for, only its own time
        atomic_store(&stop_began, 1);
        usleep((useconds_t)atomic_load(&stop_ms_on_own_thread) * 1000);
        return noErr;
    }
    atomic_fetch_add(&stops_elsewhere, 1);
    atomic_store(&listener_asked, 1);
    double start = now();
    turns_within(queue_run_loop, 2.0);
    long waited = (long)((now() - start) * 1000);
    if (waited > atomic_load(&longest_stop_ms))
        atomic_store(&longest_stop_ms, waited);
    return noErr;
}
static OSStatus fake_set_property(AudioQueueRef queue, AudioQueuePropertyID id, const void *data, UInt32 size) {
    CHECK(queue == Queue && id == kAudioQueueProperty_CurrentDevice && size == sizeof(CFStringRef));
    CHECK(CFStringCompare(*(const CFStringRef *)data, CFSTR("BO3MacFix test output"), 0) == kCFCompareEqualTo);
    atomic_fetch_add(&set_devices, 1);
    return noErr;
}
static OSStatus fake_enqueue(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 descriptions,
                             const AudioStreamPacketDescription *description) {
    CHECK(queue == Queue && buffer != NULL && descriptions == 0 && description == NULL);
    CHECK(buffer->mAudioDataByteSize == buffer->mAudioDataBytesCapacity && ((uint8_t *)buffer->mAudioData)[0] == 0);
    atomic_fetch_add(&enqueued, 1);
    return noErr;
}
static OSStatus fake_start(AudioQueueRef queue, const AudioTimeStamp *start) {
    CHECK(queue == Queue && start == NULL);
    atomic_fetch_add(&starts, 1);
    return noErr;
}
static CFStringRef fake_default_uid(void) {
    return CFStringCreateCopy(NULL, CFSTR("BO3MacFix test output"));
}

// ---- the made-up SDL audio thread

enum { Running, Setup, Gone };
static int phase;
static atomic_int registered, turning, closing;
static uint8_t buffer_bytes[3][64];
// (CoreAudio's buffer has its capacity and data as const members: set where they are made)
static AudioQueueBuffer buffer_records[3] = {
    { .mAudioDataBytesCapacity = sizeof(buffer_bytes[0]), .mAudioData = buffer_bytes[0] },
    { .mAudioDataBytesCapacity = sizeof(buffer_bytes[1]), .mAudioData = buffer_bytes[1] },
    { .mAudioDataBytesCapacity = sizeof(buffer_bytes[2]), .mAudioData = buffer_bytes[2] },
};

static void *sdl_audio_thread(void *unused) {
    (void)unused;
    pthread_setname_np("AudioQueue thread"); // SDL's name for it (0x10163bd3b)
    queue_run_loop = CFRunLoopGetCurrent();
    queue_made(Queue, queue_run_loop, kCFRunLoopDefaultMode); // what AudioQueueNewOutput_new does
    atomic_store(&registered, 1);
    // during the set-up, the default output changes: SDL goes on as soon as the listener has been asked
    if (phase == Setup)
        while (!atomic_load(&listener_asked))
            usleep(1000);
    for (int i = 0; i < 3; i++) {
        memset(buffer_bytes[i], 0x55, sizeof(buffer_bytes[i])); // what SDL's callback put there
        buffer_records[i].mAudioDataByteSize = 7;
        remember_buffer(Queue, &buffer_records[i]); // what AudioQueueAllocateBuffer_new keeps, under queues_lock
    }
    atomic_store(&turning, 1);
    double until = now() + (phase == Gone ? 0.2 : 4.0);
    while (!atomic_load(&closing) && now() < until)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    return NULL;
}

static void reset(int which) {
    phase = which;
    atomic_store(&stops, 0), atomic_store(&stops_elsewhere, 0), atomic_store(&set_devices, 0);
    atomic_store(&enqueued, 0), atomic_store(&starts, 0), atomic_store(&longest_stop_ms, 0);
    atomic_store(&listener_asked, 0), atomic_store(&registered, 0), atomic_store(&turning, 0);
    atomic_store(&closing, 0), atomic_store(&stop_ms_on_own_thread, 0), atomic_store(&stop_began, 0);
}

static bool wait_for(atomic_int *value, int wanted, double seconds) {
    double until = now() + seconds;
    while (atomic_load(value) != wanted && now() < until)
        usleep(1000);
    return atomic_load(value) == wanted;
}

// the listener, timed: CoreAudio calls it on a thread of its own (or the main run loop's)
static double listener(void) {
    double start = now();
    default_output_changed(kAudioObjectSystemObject, 0, NULL, NULL);
    return now() - start;
}

// While its run loop turns: the move runs on the queue's own thread, the listener waits for nothing.
static void test_running(void) {
    reset(Running);
    pthread_t thread;
    pthread_create(&thread, NULL, sdl_audio_thread, NULL);
    CHECK(wait_for(&turning, 1, 2.0) && turns_within(queue_run_loop, 2.0));
    double took = listener();
    CHECK(took < 0.5);
    CHECK(wait_for(&starts, 1, 2.0));
    CHECK(stops == 1 && stops_elsewhere == 0 && set_devices == 1 && enqueued == 3 && starts == 1);
    atomic_store(&closing, 1);
    pthread_join(thread, NULL);
    forget_queue(Queue);
}

// The default output changes while SDL sets its queue up (its next step takes queues_lock): the listener doesn't
// wait, and the move runs once the set-up is done and SDL's run loop turns. Holding queues_lock while stopping from the
// listener's thread waited there for CoreAudio's time limit (15 s in the game) and kept SDL's set-up waiting as long.
static void test_setup(void) {
    reset(Setup);
    pthread_t thread;
    pthread_create(&thread, NULL, sdl_audio_thread, NULL);
    CHECK(wait_for(&registered, 1, 2.0));
    double took = listener();
    atomic_store(&listener_asked, 1);
    CHECK(took < 0.5);
    if (took >= 0.5)
        printf("  the listener waited %.1f s\n", took);
    CHECK(wait_for(&starts, 1, 2.0));
    CHECK(stops == 1 && stops_elsewhere == 0 && set_devices == 1 && enqueued == 3 && starts == 1);
    atomic_store(&closing, 1);
    pthread_join(thread, NULL);
    forget_queue(Queue);
}

// The game's close: the queue is disposed while SDL's thread still turns its run loop, here while a move is in its
// stop, which takes half a second right after a start. The close waits for that move to end (it holds queues_lock),
// about half a second at most, and no move touches the queue after it was disposed.
static void test_close_while_turning(void) {
    reset(Running);
    pthread_t thread;
    pthread_create(&thread, NULL, sdl_audio_thread, NULL);
    CHECK(wait_for(&turning, 1, 2.0) && turns_within(queue_run_loop, 2.0));
    atomic_store(&stop_ms_on_own_thread, 300);
    CHECK(listener() < 0.5);
    CHECK(wait_for(&stop_began, 1, 2.0));
    double start = now();
    forget_queue(Queue); // what SDL's close does first (AudioQueueDispose_new's part), its thread still turning
    double waited = now() - start;
    CHECK(starts == 1);  // the move had ended when the dispose went on
    CHECK(waited < 0.6); // the rest of the move's 0.3 s, no more
    // another change after the dispose, while the thread still turns: nothing is moved any more
    CHECK(listener() < 0.5);
    CHECK(turns_within(queue_run_loop, 2.0) && turns_within(queue_run_loop, 2.0));
    CHECK(stops == 1 && starts == 1);
    atomic_store(&closing, 1);
    pthread_join(thread, NULL);
}

// SDL's thread has ended before its queue is disposed: not the game's close (above), but what an open that fails does
// (its thread ends, then the close disposes). Nothing is moved and nothing waits.
static void test_gone(void) {
    reset(Gone);
    pthread_t thread;
    pthread_create(&thread, NULL, sdl_audio_thread, NULL);
    pthread_join(thread, NULL);
    double took = listener();
    CHECK(took < 0.5);
    usleep(300000);
    CHECK(stops == 0 && starts == 0);
    double start = now();
    forget_queue(Queue); // AudioQueueDispose_new's part
    CHECK(now() - start < 0.5);
}

// The queue is disposed before its move gets to run (a change just before the game's close): the move finds nothing.
static void test_disposed_first(void) {
    reset(Setup);
    pthread_t thread;
    pthread_create(&thread, NULL, sdl_audio_thread, NULL);
    CHECK(wait_for(&registered, 1, 2.0));
    listener();
    forget_queue(Queue);
    atomic_store(&listener_asked, 1);
    CHECK(wait_for(&turning, 1, 2.0) && turns_within(queue_run_loop, 2.0));
    CHECK(stops == 0 && starts == 0);
    atomic_store(&closing, 1);
    pthread_join(thread, NULL);
}

// The test hook BO3MACFIX_AUDIOMOVEATSETUP: when SDL makes its queue, a move is asked from another thread, and the
// set-up goes on once that ask is under way. The listener returns at once, and the move runs on SDL's thread after
// the set-up, with its three buffers, once only.
static void test_hook_at_setup(void) {
    reset(Running);
    unsigned before = atomic_load(&moves_done);
    atomic_store(&move_at_setup, true);
    atomic_store(&setup_ask_ms, -1);
    pthread_t thread;
    pthread_create(&thread, NULL, sdl_audio_thread, NULL);
    CHECK(wait_for(&starts, 1, 3.0));
    CHECK(atomic_load(&setup_ask_ms) >= 0 && atomic_load(&setup_ask_ms) < 500);
    CHECK(stops == 1 && stops_elsewhere == 0 && enqueued == 3 && starts == 1);
    CHECK(atomic_load(&moves_done) == before + 1);
    CHECK(!atomic_load(&move_at_setup)); // once
    atomic_store(&closing, 1);
    pthread_join(thread, NULL);
    forget_queue(Queue);
}

// com.bo3macfix.test.audiomove with the value 2: two moves asked at once run one after the other on SDL's thread
static void test_hook_two_at_once(void) {
    reset(Running);
    unsigned before = atomic_load(&moves_done);
    pthread_t thread;
    pthread_create(&thread, NULL, sdl_audio_thread, NULL);
    CHECK(wait_for(&turning, 1, 2.0) && turns_within(queue_run_loop, 2.0));
    double start = now();
    ask_for_moves(2);
    CHECK(now() - start < 0.5);
    CHECK(wait_for(&starts, 2, 3.0));
    CHECK(stops == 2 && stops_elsewhere == 0 && set_devices == 2 && enqueued == 6 && starts == 2);
    CHECK(atomic_load(&moves_done) == before + 2);
    atomic_store(&closing, 1);
    pthread_join(thread, NULL);
    forget_queue(Queue);
}

// a queue whose callbacks come on CoreAudio's own thread isn't kept, and nothing is posted for it
static void test_without_run_loop(void) {
    reset(Running);
    remember_queue(Queue, NULL, NULL);
    for (int i = 0; i < Max_Queues; i++)
        CHECK(queues[i].queue == NULL);
    CHECK(listener() < 0.5);
    CHECK(stops == 0);
}

int main(void) {
    atomic_store(&audio_follow, true);
    queue_stop = fake_stop;
    queue_set_property = fake_set_property;
    queue_enqueue = fake_enqueue;
    queue_start = fake_start;
    copy_default_uid = fake_default_uid;
    test_running();
    test_setup();
    test_close_while_turning();
    test_gone();
    test_disposed_first();
    test_hook_at_setup();
    test_hook_two_at_once();
    test_without_run_loop();
    if (failures != 0) {
        printf("test_qol_audio: %d failed\n", failures);
        return 1;
    }
    printf("test_qol_audio: all passed\n");
    return 0;
}
