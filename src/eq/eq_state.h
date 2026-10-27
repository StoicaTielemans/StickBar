#ifndef CUSTOMBAR_EQ_EQ_STATE_H
#define CUSTOMBAR_EQ_EQ_STATE_H

#include <stdbool.h>

/*
 * Read-only mirror of the TRN Black Pearl DAC's EQ, as last written/read by
 * the DacController project's blackpearl-eq.py script -- see CLAUDE.md's
 * "EQ (TRN Black Pearl)" section for the full writeup.
 *
 * The bar deliberately never talks to the DAC itself: the script already
 * owns the DAC's HID protocol, and a second process issuing HID reads at the
 * same time as a script write can steal the DAC's replies and make that
 * write's own read-back verification fail. Instead the script writes a JSON
 * snapshot (~/.cache/blackpearl-eq/state.json, atomically via temp file +
 * rename) after every command that changes or reads the EQ, and this module
 * watches that file with inotify -- so the bar picks up a change the moment
 * it happens, with no polling at all, and costs nothing while nothing changes.
 *
 * Structurally the EQ-domain twin of network/network_client.c (plain
 * synchronous accessors over a cached snapshot, no locking), but event-driven
 * via its own inotify fd rather than piggybacking on the shared 5s timer.
 */

// Largest curve the state file is expected to carry (the script writes 64
// points); extra points beyond this are ignored rather than rejected.
#define EQ_STATE_MAX_POINTS 128

// Creates the inotify watch on the state file's directory (creating that
// directory first if it doesn't exist yet, so the watch can be armed before
// the script has ever run) and loads the current snapshot, if any. Returns
// false if inotify itself couldn't be set up -- the module still renders
// whatever was loaded at startup, it just won't live-update for this run
// (same "degrade gracefully" convention as every other optional module).
bool eq_state_init(void);

// The inotify fd to register on the main epoll loop; -1 if init failed.
int eq_state_watch_fd(void);

// Drains pending inotify events and reloads the snapshot if state.json itself
// was (re)written. Returns true if the snapshot changed (caller should redraw).
bool eq_state_pump(void);

void eq_state_shutdown(void);

// --- Snapshot accessors (all safe to call even if nothing was ever loaded) ---

bool eq_state_available(void);   // false until a valid state.json has been read
const char *eq_state_profile(void); // preset name, or "" if the EQ matches no preset
const char *eq_state_gain(void);    // "low" / "high", or "" if unknown
double eq_state_preamp(void);
bool eq_state_enabled(void);        // true unless the state file says the EQ is off
int eq_state_curve_count(void);
double eq_state_curve_freq(int i);  // Hz, log-spaced 20..20000
double eq_state_curve_db(int i);    // EQ response in dB (preamp not included)
double eq_state_peak_db(void);
double eq_state_min_db(void);

// --- Applying a profile ---

// Runs `python3 blackpearl-eq.py profile NAME` in the background (double-
// forked, stdout/stderr to /dev/null -- the bar never waits on it). The
// script writes the state file when it's done, which arrives back here via
// the inotify watch like any other change. Marks `name` as pending until then.
void eq_state_apply_profile(const char *name);

// The profile currently being applied, or "" if none (or if the last apply
// has been pending for so long that it evidently failed -- e.g. the DAC was
// unplugged, so the script exited before writing any state).
const char *eq_state_pending_profile(void);

#endif
