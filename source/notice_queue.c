// See notice_queue.h.
#include <string.h>
#include "notice_queue.h"

void notice_queue_push(notice_queue_t *queue, const char *text, double now) {
    if (queue->count > 0) {
        const notice_t *newest = &queue->items[(queue->head + queue->count - 1) % Notice_Queue_Max];
        if (strcmp(newest->text, text) == 0 && now - newest->time < 3.0)
            return;
    }
    if (queue->count == Notice_Queue_Max) {
        queue->head = (queue->head + 1) % Notice_Queue_Max;
        queue->count--;
    }
    notice_t *item = &queue->items[(queue->head + queue->count) % Notice_Queue_Max];
    strncpy(item->text, text, sizeof(item->text) - 1);
    item->text[sizeof(item->text) - 1] = '\0';
    item->time = now;
    queue->count++;
}

bool notice_queue_pop(notice_queue_t *queue, double now, double max_age, char *out, size_t out_size) {
    while (queue->count > 0) {
        const notice_t *item = &queue->items[queue->head];
        queue->head = (queue->head + 1) % Notice_Queue_Max;
        queue->count--;
        if (now - item->time <= max_age) {
            if (out_size > 0) {
                strncpy(out, item->text, out_size - 1);
                out[out_size - 1] = '\0';
            }
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
