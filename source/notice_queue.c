// See notice_queue.h.
#include <string.h>
#include "notice_queue.h"

// removes the item at position at (0 = the oldest), keeping the order of the rest
static void remove_at(notice_queue_t *queue, int at) {
    for (int i = at; i > 0; i--)
        queue->items[(queue->head + i) % Notice_Queue_Max] = queue->items[(queue->head + i - 1) % Notice_Queue_Max];
    queue->head = (queue->head + 1) % Notice_Queue_Max;
    queue->count--;
}

static void push(notice_queue_t *queue, const char *text, double now, bool lasting) {
    if (queue->count > 0) {
        const notice_t *newest = &queue->items[(queue->head + queue->count - 1) % Notice_Queue_Max];
        if (strcmp(newest->text, text) == 0 && now - newest->time < 3.0)
            return;
    }
    if (queue->count == Notice_Queue_Max) {
        int oldest = 0;
        while (oldest < queue->count && queue->items[(queue->head + oldest) % Notice_Queue_Max].lasting)
            oldest++;
        remove_at(queue, oldest < queue->count ? oldest : 0);
    }
    notice_t *item = &queue->items[(queue->head + queue->count) % Notice_Queue_Max];
    strncpy(item->text, text, sizeof(item->text) - 1);
    item->text[sizeof(item->text) - 1] = '\0';
    item->time = now;
    item->lasting = lasting;
    queue->count++;
}

void notice_queue_push(notice_queue_t *queue, const char *text, double now) {
    push(queue, text, now, false);
}

void notice_queue_push_lasting(notice_queue_t *queue, const char *text, double now) {
    push(queue, text, now, true);
}

bool notice_queue_pop(notice_queue_t *queue, double now, double max_age, char *out, size_t out_size, bool *lasting) {
    while (queue->count > 0) {
        const notice_t *item = &queue->items[queue->head];
        queue->head = (queue->head + 1) % Notice_Queue_Max;
        queue->count--;
        if (item->lasting || now - item->time <= max_age) {
            if (out_size > 0) {
                strncpy(out, item->text, out_size - 1);
                out[out_size - 1] = '\0';
            }
            if (lasting != NULL)
                *lasting = item->lasting;
            return true;
        }
    }
    return false;
}

int battery_warning(int *warned, int percent, bool charging) {
    static const int levels[] = { 20, 10, 5 };
    if (charging) {
        *warned = 0;
        return 0;
    }
    if (percent < 0)
        return 0;
    // readings wobble by a step or so (a DualSense reports tens), so only a clear rise counts as charged
    if (*warned != 0 && percent >= *warned + 20)
        *warned = 0;
    int warn = 0;
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (percent <= levels[i] && (*warned == 0 || levels[i] < *warned))
            warn = levels[i];
    }
    if (warn != 0)
        *warned = warn;
    return warn;
}
