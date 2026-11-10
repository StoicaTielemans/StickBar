#ifndef CUSTOMBAR_VISUALIZER_VISUALIZER_H
#define CUSTOMBAR_VISUALIZER_VISUALIZER_H

#include <stdbool.h>
#include <stdint.h>

#include "visualizer/spectrum.h"

// The audio spectrum graphs in the media dropdown (modules/media/media_popup.c):
// up to VISUALIZER_MAX_SOURCES PipeWire capture streams ("slots"), one per
// player card, each capturing *that player's own* playback stream in stereo
// (or, if the player's stream couldn't be found, the default output's mix).
// Analysed by spectrum.c, animated by a 15fps timerfd this file owns. A slot
// only captures while its card's player is playing and the dropdown is open;
// with the dropdown closed nothing runs at all. See docs/modules/media.md.
//
// Its own pw_thread_loop and connection, separate from pipewire/pipewire_client.c
// (that one only watches the graph and never carries audio). Same "degrade
// gracefully" convention as everything else: on failure the media dropdown
// just has no graphs for this run. Must be called after main()'s
// sigprocmask(), like every other init that starts a thread.
bool visualizer_init(void);
void visualizer_shutdown(void);

// False if init failed -- the media dropdown then leaves out the graph rows,
// so it looks exactly like it did before this existed.
bool visualizer_available(void);

// Fixed slot count, like this project's other fixed-size tables: cards past
// this many get no graph (docs/limitations.md).
#define VISUALIZER_MAX_SOURCES 4

enum { VISUALIZER_LEFT, VISUALIZER_RIGHT, VISUALIZER_CHANNELS };

// Target for a slot that should capture the default output's mix (the sink's
// monitor) instead of one app's stream.
#define VISUALIZER_TARGET_MIX 0u

// Points `slot` at `target_serial` (a playback stream node's object.serial, or
// VISUALIZER_TARGET_MIX) and turns its capture on or off. Cheap and idempotent:
// main.c (via media_popup_sync_visualizer()) calls it for every slot once per
// event-loop iteration. A changed target recreates the slot's stream; turning
// a slot off pauses its stream and lets its levels decay to rest.
void visualizer_set_source(int slot, uint32_t target_serial, bool active);

// The animation timerfd. Armed while any slot is capturing or still decaying;
// otherwise the bar does no visualizer work at all. On readiness call
// visualizer_pump().
int visualizer_timer_fd(void);

// Drains the timerfd and advances every slot by one frame: slots in
// `ready_mask` (bit = slot; their graph can show a new frame now) are
// analysed and smoothed; capturing slots that aren't ready are skipped this
// tick; inactive slots not in `shown_mask` (no graph on screen) drop straight
// to rest. Returns the mask of slots whose levels changed -- the caller
// redraws just those graphs. Disarms the timer once nothing is capturing and
// everything has settled.
uint32_t visualizer_pump(uint32_t ready_mask, uint32_t shown_mask);

// Current smoothed levels (0..1, SPECTRUM_BAR_COUNT of them) for one slot's
// channel, and its peak markers (>= the level: the recent high, held ~0.5s,
// then falling slower than the level). All zero when unavailable.
const float *visualizer_levels(int slot, int channel);
const float *visualizer_peaks(int slot, int channel);

// Test-only, with STICKBAR_TEST_VISUALIZER_SYNTH set: `slot`'s `channel`
// "plays" a sine at `hz` (0 = silence) whenever the slot is active -- instead
// of any captured audio (synth mode never connects to PipeWire). Driven by
// the test-control channel's `vis-tone` command.
void visualizer_test_set_tone(int slot, int channel, double hz);

// Every capture stream's PipeWire node.name. pipewire/pipewire_client.c skips
// these nodes so the privacy indicator doesn't report the bar itself as
// "recording" -- they're technically Stream/Input/Audio nodes like any other.
#define VISUALIZER_NODE_NAME "stickBar-visualizer"

#endif
