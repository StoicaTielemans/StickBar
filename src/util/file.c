#include "util/file.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Reads up to out_size - 1 bytes of `path` into `out`, NUL-terminated.
// Returns the byte count, or -1.
static ssize_t read_small(const char *path, char *out, size_t out_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return n;
}

bool file_read_trimmed(const char *path, char *out, size_t out_size) {
    out[0] = '\0';
    ssize_t n = read_small(path, out, out_size);
    if (n < 0) {
        out[0] = '\0';
        return false;
    }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return true;
}

bool file_read_long(const char *path, long *out) {
    char buf[64];
    if (read_small(path, buf, sizeof(buf)) <= 0) {
        return false;
    }
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf) {
        return false;
    }
    *out = v;
    return true;
}

bool proc_read_ppid(int pid, int *ppid) {
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_small(path, buf, sizeof(buf)) <= 0) {
        return false;
    }
    // "pid (comm) state ppid ..." -- comm can itself contain spaces and
    // parentheses, so parse from the *last* ')'.
    char *close = strrchr(buf, ')');
    return close && sscanf(close + 1, " %*c %d", ppid) == 1;
}
