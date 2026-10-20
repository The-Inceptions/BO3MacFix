// See checkpoint_autosave.h.
#include <dispatch/dispatch.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "checkpoint_autosave.h"

static pthread_mutex_t checkpoint_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static atomic_bool enabled = true;

// the writer: the save it hasn't written yet, and whether its job is queued (waiting for the interval, or running)
static pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t writer_idle = PTHREAD_COND_INITIALIZER;
static char *pending_lines, *pending_note; // under writer_mutex, NULL for none
static bool job_queued;                    // under writer_mutex
static uint64_t last_save_ns;              // under writer_mutex: when the writer last dealt with a save, 0 for never
static dispatch_queue_t queue;             // under writer_mutex, made once
static checkpoint_paths_t paths_now;       // under writer_mutex
static int interval_ms;                    // under writer_mutex
static autosave_report_t report_now;       // under writer_mutex

void checkpoint_lock(void) {
    pthread_mutex_lock(&checkpoint_mutex);
}

bool checkpoint_trylock(void) {
    return pthread_mutex_trylock(&checkpoint_mutex) == 0;
}

void checkpoint_unlock(void) {
    pthread_mutex_unlock(&checkpoint_mutex);
}

// the whole number a "key=value" line in lines gives key (checkpoint_read's and checkpoint.lua's lines)
static bool line_number(const char *lines, const char *key, long *out) {
    size_t len = strlen(key);
    for (const char *line = lines; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        if (strncmp(line, key, len) == 0 && line[len] == '=') {
            const char *value = line + len + 1;
            char *stop = NULL;
            errno = 0;
            long number = strtol(value, &stop, 10);
            bool whole = stop != value && errno == 0 && (*stop == '\0' || *stop == '\n' || *stop == '\r') &&
                         *value >= '0' && *value <= '9';
            if (whole)
                *out = number;
            return whole;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return false;
}

bool checkpoint_may_replace(const char *saved, long round, bool resumed) {
    long kept = 0;
    return resumed || saved == NULL || !line_number(saved, "Round", &kept) || kept <= round;
}

void checkpoint_autosave_set_enabled(bool on) {
    atomic_store(&enabled, on);
}

bool checkpoint_autosave_enabled(void) {
    return atomic_load(&enabled);
}

// saves lines unless the rules say otherwise; holding the checkpoint lock
static autosave_outcome_t save(const checkpoint_paths_t *paths, const char *lines, long *round, long *kept) {
    static char saved[Checkpoint_Read_Size];
    *round = 0;
    *kept = 0;
    if (!line_number(lines, "Round", round))
        return Autosave_Failed;
    checkpoint_resume_t state = checkpoint_resume_state(paths);
    // the resume gives the saved game's weapons back from checkpoint.ini, so that has to keep them until it has
    if (state == Checkpoint_Resume_In_Use)
        return Autosave_Resuming;
    checkpoint_read(paths, saved, sizeof(saved));
    line_number(saved, "Round", kept);
    if (!checkpoint_may_replace(saved, *round, state == Checkpoint_Resume_Weapons_Given))
        return Autosave_Kept_Deeper;
    return checkpoint_save(paths, lines, (long)time(NULL)) ? Autosave_Saved : Autosave_Failed;
}

static void write_job(void *context);

// queues the writer's job for when the interval since its last save is up; holding writer_mutex
static void schedule(void) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t due = last_save_ns != 0 ? last_save_ns + (uint64_t)interval_ms * NSEC_PER_MSEC : 0;
    if (due > now)
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(due - now)), queue, NULL, write_job);
    else
        dispatch_async_f(queue, NULL, write_job);
}

// on the writer's queue: the newest save it was given, if a manual save or a delete hasn't replaced it
static void write_job(void *context) {
    (void)context;
    checkpoint_lock();
    pthread_mutex_lock(&writer_mutex);
    char *lines = pending_lines, *note = pending_note;
    pending_lines = pending_note = NULL;
    checkpoint_paths_t paths = paths_now;
    autosave_report_t report = report_now;
    pthread_mutex_unlock(&writer_mutex);
    long round = 0, kept = 0;
    autosave_outcome_t outcome = lines != NULL ? save(&paths, lines, &round, &kept) : Autosave_Failed;
    checkpoint_unlock();
    if (lines != NULL && report != NULL)
        report(outcome, round, kept, note != NULL ? note : "");
    pthread_mutex_lock(&writer_mutex);
    if (lines != NULL)
        last_save_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (pending_lines != NULL) {
        schedule();
    } else {
        job_queued = false;
        pthread_cond_broadcast(&writer_idle);
    }
    pthread_mutex_unlock(&writer_mutex);
    free(lines);
    free(note);
}

void checkpoint_autosave_init(const checkpoint_paths_t *paths, int min_interval_ms, autosave_report_t report) {
    pthread_mutex_lock(&writer_mutex);
    paths_now = *paths;
    interval_ms = min_interval_ms > 0 ? min_interval_ms : 0;
    report_now = report;
    // below the game's own threads, but not so low that the writer could be kept waiting holding the lock the UI's
    // thread wants (its work is a few small file operations)
    if (queue == NULL)
        queue = dispatch_queue_create("com.bo3macfix.autosave", dispatch_queue_attr_make_with_qos_class(
                                                                    DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    pthread_mutex_unlock(&writer_mutex);
}

bool checkpoint_autosave_post(const char *lines, const char *note) {
    if (!atomic_load(&enabled) || lines == NULL)
        return false;
    char *lines_copy = strdup(lines), *note_copy = strdup(note != NULL ? note : "");
    pthread_mutex_lock(&writer_mutex);
    bool posted = queue != NULL && lines_copy != NULL && note_copy != NULL;
    if (posted) {
        free(pending_lines);
        free(pending_note);
        pending_lines = lines_copy;
        pending_note = note_copy;
        lines_copy = note_copy = NULL;
        if (!job_queued) {
            job_queued = true;
            schedule();
        }
    }
    pthread_mutex_unlock(&writer_mutex);
    free(lines_copy);
    free(note_copy);
    return posted;
}

void checkpoint_autosave_cancel(void) {
    pthread_mutex_lock(&writer_mutex);
    free(pending_lines);
    free(pending_note);
    pending_lines = pending_note = NULL;
    pthread_mutex_unlock(&writer_mutex);
}

void checkpoint_autosave_flush(void) {
    pthread_mutex_lock(&writer_mutex);
    while (job_queued)
        pthread_cond_wait(&writer_idle, &writer_mutex);
    pthread_mutex_unlock(&writer_mutex);
}

bool checkpoint_delete(const checkpoint_paths_t *paths) {
    checkpoint_lock();
    checkpoint_autosave_cancel();
    // the resume's record of the player's own AAE options is in checkpoint.ini
    bool deleted = checkpoint_resume_state(paths) == Checkpoint_No_Resume &&
                   (unlink(paths->checkpoint) == 0 || errno == ENOENT);
    checkpoint_unlock();
    return deleted;
}
