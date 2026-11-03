#ifndef CUSTOMBAR_TESTING_TEST_CONTROL_H
#define CUSTOMBAR_TESTING_TEST_CONTROL_H

#include <stdbool.h>

#include "wayland/context.h"

/*
 * Test-control channel: lets tests/ drive a bar instance -- open dropdowns,
 * click/drag/scroll inside them, read what its data sources hold, and render
 * the open dropdown to a PNG -- without a real pointer and without screen
 * capture. Off unless the environment has STICKBAR_TEST_CONTROL=1; in normal
 * use none of this runs and stdin is never read.
 *
 * Protocol: one command per line on stdin, exactly one JSON object per line
 * back on stdout ({"ok":false,"error":...} for anything it can't do). See
 * test_control.c's dispatch() for the command list, and tests/lib/common.py's
 * ControlledBar for the Python side.
 *
 * Injected input goes through wayland/pointer.c's own routing
 * (pointer_dispatch_button() etc.), so it exercises the same hit-testing and
 * popup code a real click does -- what it skips is only the compositor
 * deciding which surface is under the cursor.
 */

// True when STICKBAR_TEST_CONTROL=1. Surfaces created in that mode get an
// empty input region (test_control_isolate_surface()), so the user's real
// pointer passes straight through a test instance -- it can't steal their
// clicks, and their clicks can't close a test popup mid-test.
bool test_control_enabled(void);
void test_control_isolate_surface(AppContext *app, struct wl_surface *surface);

// Returns the fd to watch (stdin) if the channel is enabled, else -1.
int test_control_init(AppContext *app);

// Reads and runs every complete command line available on the fd. Returns
// false once the other end has closed it -- the caller should then stop
// watching the fd (epoll would otherwise report EOF forever).
bool test_control_pump(void);

#endif
