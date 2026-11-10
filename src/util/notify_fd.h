#ifndef CUSTOMBAR_UTIL_NOTIFY_FD_H
#define CUSTOMBAR_UTIL_NOTIFY_FD_H

// An eventfd a background thread (pulse_client.c's, pipewire_client.c's)
// signals to wake the main epoll loop when its cached state changed. The
// main thread registers it on epoll and drains it on readiness.

// -1 (with a perror naming `what`) on failure.
int notify_fd_new(const char *what);

// Safe from any thread: eventfd_write() touches no library state, so it can
// be called from inside a libpulse/PipeWire callback with their lock held.
void notify_fd_signal(int fd);

// One read drains the whole accumulated count (eventfd's default,
// non-semaphore mode), however many signals piled up.
void notify_fd_drain(int fd);

#endif
