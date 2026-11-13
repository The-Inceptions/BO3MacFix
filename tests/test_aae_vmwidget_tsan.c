// aae_vmwidget.c under the thread sanitizer: two writers of one bone at once (the view model's two DObjCalcSkel
// calls on two job threads, or a tag's first widget on the main thread while a job thread writes) must not leave its
// sequence count odd, which would keep the widget on that bone from ever moving again.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// where a writer is between taking the bone (its count odd) and its copy: the test's chance to start the other one
static void while_writing(const void *bone);
#define VMWIDGET_WHILE_WRITING(bone) while_writing(bone)

#include "../source/aae_plugin.c"
#include "../source/aae_vmwidget.c"

// ---- what the code asks of the rest of BO3MacFix (nothing of it is used here)
const char *t7o_mod_folder(void) {
    return "";
}
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}
void aae_skeleton_add(uint64_t game_base_address, int piece, aae_skeleton_work_t work) {
    (void)game_base_address, (void)piece, (void)work;
}

// ---- a made-up weapon model with the bone the tag is on
#define Bone_Index 3
static uint8_t model[0x200], tree[16];
static float skeleton[8][8];
static aae_vmwidget_tag_t *tag;
static int bone_index(const uint8_t *object, uint32_t name, int16_t *index, uint32_t parts) {
    (void)object, (void)name, (void)parts;
    *index = Bone_Index;
    return 1;
}
static void a_model(void) {
    memset(&state, 0, sizeof(state));
    const void *tree_pointer = tree, *skeleton_pointer = skeleton;
    uint16_t bones = 8;
    uint32_t computed = 0x80000000u >> Bone_Index;
    memcpy(model + Model_Tree, &tree_pointer, sizeof(tree_pointer));
    memcpy(model + Model_Skeleton, &skeleton_pointer, sizeof(skeleton_pointer));
    memcpy(model + Model_Bones, &bones, sizeof(bones));
    memcpy(model + Model_Computed, &computed, sizeof(computed));
    const float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f }, position[3] = { 10.0f, 20.0f, 30.0f };
    memcpy(skeleton[Bone_Index], rotation, sizeof(rotation));
    memcpy(skeleton[Bone_Index] + 4, position, sizeof(position));
    tag = aae_vmwidget_tag(&state, 0x1234);
    aae_vmwidget_tag_t *user = NULL;
    aae_vmwidget_move(&state, &user, tag);
    aae_vmwidget_note(&state, 0, model);
    // the bone looked for once here, so that the threads below only read the cache
    aae_vmwidget_skeleton(&state, model, bone_index);
}

// ---- two writers at once, in the order that leaves the plugin's count odd: B reads A's odd count, A finishes, then
// B finishes. A is the skeleton's copy, B an invalidation (a note, or a tag's first widget).
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local int role; // 1 for A, 2 for B, 0 for anything else
static bool staging; // only the staged run coordinates; the stress runs don't
static enum { Nothing, A_Writing, B_Writing, B_Done, A_Done } stage;
static void set_stage(int to) {
    pthread_mutex_lock(&lock);
    stage = to;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}
static void wait_for(bool (*until)(void)) {
    pthread_mutex_lock(&lock);
    while (!until())
        pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}
static bool b_writing_or_done(void) {
    return stage == B_Writing || stage == B_Done;
}
static bool a_done(void) {
    return stage == A_Done;
}
static bool a_writing(void) {
    return stage == A_Writing;
}
static void while_writing(const void *bone) {
    (void)bone;
    if (!staging)
        return;
    if (role == 1) {
        // A holds the bone: B starts now and either gives way or gets as far as its own copy
        set_stage(A_Writing);
        wait_for(b_writing_or_done);
    } else if (role == 2) {
        // B got the bone as well (the plugin's way): it finishes after A
        set_stage(B_Writing);
        wait_for(a_done);
    }
}
static void *writer_a(void *unused) {
    (void)unused;
    role = 1;
    aae_vmwidget_skeleton(&state, model, bone_index);
    set_stage(A_Done);
    return NULL;
}
static void *writer_b(void *unused) {
    (void)unused;
    role = 2;
    wait_for(a_writing);
    invalidate(&tag->bone[0]);
    pthread_mutex_lock(&lock);
    if (stage != A_Done)
        stage = B_Done;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void test_staged(void) {
    a_model();
    uint32_t before = tag->bone[0].sequence;
    CHECK((before & 1) == 0 && tag->bone[0].valid == 1);
    staging = true;
    stage = Nothing;
    pthread_t a, b;
    pthread_create(&b, NULL, writer_b, NULL);
    pthread_create(&a, NULL, writer_a, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    staging = false;
    // B gave way: the count moved on by A's two steps, even, and A's copy stands, readable
    aae_vmwidget_bone_t copy;
    CHECK((tag->bone[0].sequence & 1) == 0);
    CHECK(tag->bone[0].sequence == before + 2);
    CHECK(aae_vmwidget_read(tag, 0, &copy) && copy.position[0] == 10.0f && copy.position[2] == 30.0f);
    // and the next writer gets it as usual
    invalidate(&tag->bone[0]);
    CHECK(tag->bone[0].sequence == before + 4 && tag->bone[0].valid == 0);
}

// ---- many writers at once, copies and invalidations mixed: at the end the count is even, and one more copy can be
// read
#define Threads 4
#define Rounds 20000
static void *stress(void *which) {
    for (int round = 0; round < Rounds; round++) {
        if (((uintptr_t)which + (uintptr_t)round) % 3 == 0)
            invalidate(&tag->bone[0]);
        else
            aae_vmwidget_skeleton(&state, model, bone_index);
    }
    return NULL;
}

static void test_stress(void) {
    a_model();
    pthread_t threads[Threads];
    for (uintptr_t i = 0; i < Threads; i++)
        pthread_create(&threads[i], NULL, stress, (void *)i);
    for (int i = 0; i < Threads; i++)
        pthread_join(threads[i], NULL);
    CHECK((tag->bone[0].sequence & 1) == 0);
    aae_vmwidget_skeleton(&state, model, bone_index);
    aae_vmwidget_bone_t copy;
    CHECK(aae_vmwidget_read(tag, 0, &copy) && copy.position[1] == 20.0f);
}

int main(void) {
    ready = true;
    test_staged();
    test_stress();
    if (failures != 0) {
        printf("test_aae_vmwidget_tsan: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_vmwidget_tsan: all passed\n");
    return 0;
}
