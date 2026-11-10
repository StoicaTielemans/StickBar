#ifndef CUSTOMBAR_UTIL_TIMER_H
#define CUSTOMBAR_UTIL_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// timerfd helpers shared by every timer in the bar (main.c's clock and 5s
// poll, the D-Bus timeout, the visualizer's frame timer).

// A non-blocking, close-on-exec timerfd on `clock`; -1 (with a perror naming
// `what`) on failure.
int timer_fd_new(clockid_t clock, const char *what);

// Arms `fd` to fire `first_ns` from now and then every `interval_ns` (0 =
// once). first_ns 0 disarms it.
bool timer_fd_set(int fd, uint64_t first_ns, uint64_t interval_ns);

// Arms `fd` to fire at the absolute time `at_ns` on its own clock, then every
// `interval_ns` (0 = once). A time already past fires immediately. `flags`
// adds to TFD_TIMER_ABSTIME (e.g. TFD_TIMER_CANCEL_ON_SET).
bool timer_fd_set_abs(int fd, uint64_t at_ns, uint64_t interval_ns, int flags);

// Reads (drains) the expiration count -- required on every wakeup, or the fd
// stays readable forever. How many expirations there were never matters
// here. False if the read failed, with errno set (ECANCELED: a
// TFD_TIMER_CANCEL_ON_SET timer's wall clock was set).
bool timer_fd_drain(int fd);

#endif
