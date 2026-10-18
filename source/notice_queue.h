// The plain C half of BO3MacFix's on-screen notices (notices.m): the queue the game's UI takes them from, and when a
// controller's battery level is worth a warning. No game or macOS calls, so tests/test_notice_queue.c tests it as is.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define Notice_Text_Max 128
#define Notice_Queue_Max 8

typedef struct {
    char text[Notice_Text_Max];
    double time; // seconds, any clock that only goes forward
} notice_t;

typedef struct {
    notice_t items[Notice_Queue_Max];
    int head, count;
} notice_queue_t;

// adds a notice. When the queue is full the oldest goes; the same text as the newest within 3 seconds is dropped.
void notice_queue_push(notice_queue_t *queue, const char *text, double now);

// takes the oldest notice that's at most max_age seconds old into out, dropping older ones. False if there's none.
bool notice_queue_pop(notice_queue_t *queue, double now, double max_age, char *out, size_t out_size);

// Low battery warnings for one controller, at 20, 10 and 5 percent, each once. warned keeps the lowest level warned
// about (start it at 0). percent < 0 means unknown. Returns the level to warn about now, or 0. Charging, or the level
// getting back 20 points above the last warning, starts over.
int battery_warning(int *warned, int percent, bool charging);
