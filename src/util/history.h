#ifndef CUSTOMBAR_UTIL_HISTORY_H
#define CUSTOMBAR_UTIL_HISTORY_H

// A ring buffer of the last HISTORY_LEN samples of two series (usage and
// temperature, rx and tx, used and swap), one sample per 5s poll -- the
// history behind every stat dropdown's graph. Recorded always, not only
// while a dropdown is open, so a graph is already full the moment it opens:
// it costs two stores per poll on a timer that runs anyway. Header-only, the
// calls are tiny.

// 10 minutes at one sample per 5s poll; the graphs show as much as fits.
#define HISTORY_LEN 120

typedef struct {
    double a[HISTORY_LEN], b[HISTORY_LEN];
    int head; // the slot the *next* sample goes in
    int len;  // samples held, up to HISTORY_LEN
} History2;

static inline void history2_reset(History2 *h) {
    h->head = 0;
    h->len = 0;
}

static inline void history2_push(History2 *h, double a, double b) {
    h->a[h->head] = a;
    h->b[h->head] = b;
    h->head = (h->head + 1) % HISTORY_LEN;
    if (h->len < HISTORY_LEN) {
        h->len++;
    }
}

// Index 0 is the oldest sample still held: it sits at `head` once the buffer
// has wrapped, and at slot 0 before that. Out-of-range indexes read 0.
static inline int history2_slot(const History2 *h, int index) {
    int oldest = h->len < HISTORY_LEN ? 0 : h->head;
    return (oldest + index) % HISTORY_LEN;
}
static inline double history2_a(const History2 *h, int index) {
    return (index >= 0 && index < h->len) ? h->a[history2_slot(h, index)] : 0.0;
}
static inline double history2_b(const History2 *h, int index) {
    return (index >= 0 && index < h->len) ? h->b[history2_slot(h, index)] : 0.0;
}

#endif
