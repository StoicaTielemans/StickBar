#include "util/notify_fd.h"

#include <stdio.h>
#include <sys/eventfd.h>

int notify_fd_new(const char *what) {
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "stickBar: eventfd(%s): ", what);
        perror(NULL);
    }
    return fd;
}

void notify_fd_signal(int fd) {
    if (fd >= 0) {
        eventfd_write(fd, 1);
    }
}

void notify_fd_drain(int fd) {
    if (fd >= 0) {
        eventfd_t val;
        (void)eventfd_read(fd, &val);
    }
}
