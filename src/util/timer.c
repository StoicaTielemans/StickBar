#include "util/timer.h"

#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define NS_PER_S 1000000000ULL

static struct timespec to_timespec(uint64_t ns) {
    return (struct timespec){.tv_sec = (time_t)(ns / NS_PER_S), .tv_nsec = (long)(ns % NS_PER_S)};
}

int timer_fd_new(clockid_t clock, const char *what) {
    int fd = timerfd_create(clock, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "stickBar: timerfd_create(%s): ", what);
        perror(NULL);
    }
    return fd;
}

bool timer_fd_set(int fd, uint64_t first_ns, uint64_t interval_ns) {
    struct itimerspec spec = {.it_value = to_timespec(first_ns), .it_interval = to_timespec(interval_ns)};
    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
        perror("stickBar: timerfd_settime");
        return false;
    }
    return true;
}

bool timer_fd_set_abs(int fd, uint64_t at_ns, uint64_t interval_ns, int flags) {
    // An all-zero it_value would disarm the timer instead of firing it; any
    // time in the past fires immediately, so 1ns stands in for "now".
    struct itimerspec spec = {.it_value = to_timespec(at_ns ? at_ns : 1), .it_interval = to_timespec(interval_ns)};
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME | flags, &spec, NULL) < 0) {
        perror("stickBar: timerfd_settime");
        return false;
    }
    return true;
}

bool timer_fd_drain(int fd) {
    uint64_t expirations;
    return read(fd, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations);
}
