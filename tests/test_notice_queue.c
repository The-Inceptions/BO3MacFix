// Tests for notice_queue.c: the notices queue and the low battery warning levels.
#include <stdio.h>
#include <string.h>
#include "../source/notice_queue.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_queue_order_and_age(void) {
    notice_queue_t queue = { 0 };
    char out[Notice_Text_Max];
    CHECK(!notice_queue_pop(&queue, 0, 20, out, sizeof(out)));
    notice_queue_push(&queue, "one", 1);
    notice_queue_push(&queue, "two", 2);
    notice_queue_push(&queue, "three", 30);
    // 21.5 s in, "one" is over 20 s old and goes, "two" isn't
    CHECK(notice_queue_pop(&queue, 21.5, 20, out, sizeof(out)) && strcmp(out, "two") == 0);
    CHECK(notice_queue_pop(&queue, 31, 20, out, sizeof(out)) && strcmp(out, "three") == 0);
    CHECK(!notice_queue_pop(&queue, 31, 20, out, sizeof(out)));
}

static void test_queue_full_and_duplicates(void) {
    notice_queue_t queue = { 0 };
    char out[Notice_Text_Max], name[16];
    for (int i = 0; i < Notice_Queue_Max + 3; i++) {
        snprintf(name, sizeof(name), "n%d", i);
        notice_queue_push(&queue, name, i);
    }
    CHECK(queue.count == Notice_Queue_Max);
    // the oldest three went
    CHECK(notice_queue_pop(&queue, 11, 100, out, sizeof(out)) && strcmp(out, "n3") == 0);

    notice_queue_t again = { 0 };
    notice_queue_push(&again, "DualSense connected", 10);
    notice_queue_push(&again, "DualSense connected", 11);   // a repeat right away is dropped
    notice_queue_push(&again, "DualSense connected", 14.5); // later it's news again
    CHECK(again.count == 2);

    // long texts are cut, not overrun
    notice_queue_t big = { 0 };
    char text[300];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    notice_queue_push(&big, text, 0);
    CHECK(notice_queue_pop(&big, 0, 1, out, 10) && strlen(out) == 9);
}

static void test_battery_warnings(void) {
    int warned = 0;
    CHECK(battery_warning(&warned, -1, false) == 0); // unknown
    CHECK(battery_warning(&warned, 100, false) == 0);
    CHECK(battery_warning(&warned, 30, false) == 0);
    CHECK(battery_warning(&warned, 20, false) == 20);
    CHECK(battery_warning(&warned, 20, false) == 0); // once
    CHECK(battery_warning(&warned, 30, false) == 0); // a wobble up doesn't start over
    CHECK(battery_warning(&warned, 20, false) == 0);
    CHECK(battery_warning(&warned, 10, false) == 10);
    CHECK(battery_warning(&warned, 10, false) == 0);
    CHECK(battery_warning(&warned, 0, false) == 5);
    CHECK(battery_warning(&warned, 0, false) == 0);
    // charging starts over
    CHECK(battery_warning(&warned, 0, true) == 0 && warned == 0);
    CHECK(battery_warning(&warned, 15, false) == 20);

    // connected already low: one warning, the most urgent
    int fresh = 0;
    CHECK(battery_warning(&fresh, 10, false) == 10);
    CHECK(battery_warning(&fresh, 20, false) == 0);
    // charged without us seeing it: back 20 points up starts over
    CHECK(battery_warning(&fresh, 30, false) == 0 && fresh == 0);
    CHECK(battery_warning(&fresh, 20, false) == 20);
}

int main(void) {
    test_queue_order_and_age();
    test_queue_full_and_duplicates();
    test_battery_warnings();
    if (failures == 0)
        printf("test_notice_queue: all passed\n");
    return failures == 0 ? 0 : 1;
}
