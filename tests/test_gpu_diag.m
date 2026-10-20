// gpu_diag.m's command buffer timing (FrameStats detail, DynamicResolution) against a stand-in command buffer class:
// the swizzled commit calls the original and counts the GPU time, and other threads committing while the swizzle goes
// in never reach a commit with no original to call (they did: the original was stored only after
// method_setImplementation returned, and the new commit is reachable before that, while it flushes the method caches).
#include "../source/gpu_diag.m"
#include <pthread.h>

// metalfx.m's, which log_state uses
void present_describe(char *buf, size_t n) {
    if (n > 0)
        buf[0] = '\0';
}

static atomic_int original_commits = 0, handlers_added = 0;
static void (^last_handler)(id) = nil;

@interface FakeCommandBuffer : NSObject
@property CFTimeInterval GPUStartTime;
@property CFTimeInterval GPUEndTime;
- (void)commit;
- (void)addCompletedHandler:(void (^)(id))handler;
@end

@implementation FakeCommandBuffer
- (void)commit {
    atomic_fetch_add(&original_commits, 1);
}
- (void)addCompletedHandler:(void (^)(id))handler {
    atomic_fetch_add(&handlers_added, 1);
    if (pthread_main_np()) {
        if (last_handler != nil)
            Block_release(last_handler);
        last_handler = Block_copy(handler);
    }
}
@end

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static atomic_bool stop = false, started = false;

// another thread of the game committing its own command buffers all the while
static void *committer(void *arg) {
    FakeCommandBuffer *buffer = (FakeCommandBuffer *)arg;
    atomic_store(&started, true);
    while (!atomic_load(&stop))
        [buffer commit];
    return NULL;
}

int main(void) {
    @autoreleasepool {
        IMP original = method_getImplementation(class_getInstanceMethod([FakeCommandBuffer class], @selector(commit)));
        FakeCommandBuffer *other = [[FakeCommandBuffer alloc] init];
        FakeCommandBuffer *buffer = [[FakeCommandBuffer alloc] init];

        // the swizzle goes in while another thread commits (a crash, calling NULL, if the original isn't stored first)
        pthread_t thread;
        pthread_create(&thread, NULL, committer, other);
        while (!atomic_load(&started))
            ;
        gpu_diag_watch_command_buffers(buffer);
        int before = atomic_load(&original_commits);
        while (atomic_load(&original_commits) < before + 1000)
            ;
        atomic_store(&stop, true);
        pthread_join(thread, NULL);

        CHECK(orig_commit == original);
        CHECK(method_getImplementation(class_getInstanceMethod([FakeCommandBuffer class], @selector(commit))) ==
              (IMP)commit_new);
        // the other thread's commits after the swizzle went through the new commit, to the original
        CHECK(atomic_load(&handlers_added) > 0);

        // a commit: the original runs, and the handler adds the GPU time once the buffer completes
        atomic_store(&original_commits, 0);
        atomic_store(&handlers_added, 0);
        uint64_t buffers = 0;
        gpu_diag_take_busy(&buffers);
        double total_before = gpu_diag_busy_total();
        [buffer commit];
        CHECK(atomic_load(&original_commits) == 1);
        CHECK(atomic_load(&handlers_added) == 1);
        CHECK(last_handler != nil);
        buffer.GPUStartTime = 1e9;
        buffer.GPUEndTime = 1e9 + 0.004;
        if (last_handler != nil)
            last_handler(buffer);
        double busy = gpu_diag_take_busy(&buffers);
        CHECK(fabs(busy - 0.004) < 1e-6);
        CHECK(buffers == 1);
        CHECK(fabs(gpu_diag_busy_total() - total_before - 0.004) < 1e-6);

        // only the first call swizzles
        gpu_diag_watch_command_buffers(buffer);
        CHECK(orig_commit == original);
        [buffer release];
        [other release];
    }
    if (failures != 0) {
        printf("test_gpu_diag: %d failed\n", failures);
        return 1;
    }
    printf("test_gpu_diag: all passed\n");
    return 0;
}
