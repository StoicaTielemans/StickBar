#ifndef CUSTOMBAR_UTIL_FILE_H
#define CUSTOMBAR_UTIL_FILE_H

#include <stdbool.h>
#include <stddef.h>

// Small /proc and /sys reading helpers, shared by sysmon, network and
// PipeWire code that each had their own copies.

// Seconds on CLOCK_MONOTONIC, as a double -- for rate/elapsed maths.
double monotonic_seconds(void);

// Reads a small text file (a /sys one-liner) into `out`, trimming trailing
// whitespace. False -- `out` left empty -- if it doesn't exist or can't be
// read, which is normal for some (e.g. "speed" on a Wi-Fi interface). Plain
// open()/read(): no stdio buffer allocated per read, which matters for the
// files read every poll.
bool file_read_trimmed(const char *path, char *out, size_t out_size);

// A file holding one integer (a /sys value like a frequency in kHz or a
// temperature in millidegrees). False if missing or not a number.
bool file_read_long(const char *path, long *out);

// `pid`'s parent process id, from /proc/<pid>/stat. False if it's gone.
bool proc_read_ppid(int pid, int *ppid);

#endif
