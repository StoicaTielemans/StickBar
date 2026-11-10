#include "visualizer/visualizer.h"

#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

// Fixed capture format: stereo float at 48kHz, interleaved FL/FR. PipeWire's
// stream adapter converts whatever the source really runs at into this (a
// mono app is upmixed to both channels), so spectrum.c can rely on one rate.
#define CAPTURE_RATE 48000

// 15fps (was 30; halved per user request to save CPU -- see
// docs/modules/media.md for the measured difference). Each tick only redraws
// the graphs' own small strip subsurfaces (bar/strip_surface.c).
#define VISUALIZER_FPS 15
#define FRAME_NS (1000000000L / VISUALIZER_FPS)

// Per-frame smoothing toward the newest raw levels: rise most of the way at
// once (so beats land on the beat), fall at a fixed speed (so the graph drops
// smoothly instead of flickering). Both are tied to VISUALIZER_FPS so changing
// the frame rate keeps the same feel: ATTACK 0.84 per 15fps frame is the old
// 0.6 per 30fps frame applied twice (1 - 0.4^2); the fall empties a full
// column in ~0.7s at any rate.
#define ATTACK 0.84f
#define FALL_PER_SECOND 1.5f
#define FALL_PER_FRAME (FALL_PER_SECOND / VISUALIZER_FPS)

// Peak markers (the lone dot riding above each column, as in cava): a new
// high is held for PEAK_HOLD_SECONDS, then falls at PEAK_FALL_PER_SECOND --
// slower than the levels, so the markers visibly trail them down.
#define PEAK_HOLD_SECONDS 0.5f
#define PEAK_FALL_PER_SECOND 0.6f
#define PEAK_HOLD_FRAMES ((int)(PEAK_HOLD_SECONDS * VISUALIZER_FPS + 0.5f))
#define PEAK_FALL_PER_FRAME (PEAK_FALL_PER_SECOND / VISUALIZER_FPS)

// After this many frames with no new samples, a slot's rings are treated as
// silence. Needed because "Playing" doesn't mean audio is flowing: the app may
// have closed its stream (between tracks, a stalled network stream), and then
// PipeWire simply stops calling on_process() -- the last window would stay
// frozen in the rings, and the graph with it. 2 frames = ~133ms at 15fps,
// several buffer periods, so ordinary scheduling jitter never trips it.
#define STALE_FRAMES 2

typedef struct {
    int index;
    struct pw_stream *stream; // NULL until the slot is first turned on
    struct spa_hook listener;
    uint32_t target;          // what `stream` was created for
    bool active;

    // Guarded by the thread loop's lock (PipeWire calls on_process() with it
    // held, the same model pipewire_client.c relies on): the newest
    // SPECTRUM_FFT_SIZE samples per channel, as rings with `pos` the next
    // write slot (= the oldest sample).
    float ring[VISUALIZER_CHANNELS][SPECTRUM_FFT_SIZE];
    unsigned pos;
    uint32_t fresh; // frames written since the last pump

    // Main-thread only.
    int stale_frames;
    float levels[VISUALIZER_CHANNELS][SPECTRUM_BAR_COUNT];
    float peaks[VISUALIZER_CHANNELS][SPECTRUM_BAR_COUNT];
    int peak_hold[VISUALIZER_CHANNELS][SPECTRUM_BAR_COUNT]; // frames before a peak starts falling
    bool settled; // every level and peak is 0

    double synth_hz[VISUALIZER_CHANNELS]; // test-only synthetic tone per channel, 0 = silence
} Source;

static struct pw_thread_loop *g_loop = NULL;
static struct pw_context *g_context = NULL;
static struct pw_core *g_core = NULL;
static int g_timer_fd = -1;
static bool g_available = false;
static bool g_timer_armed = false;
static Source g_sources[VISUALIZER_MAX_SOURCES];

// Test-only (STICKBAR_TEST_VISUALIZER_SYNTH, set by tests/test_visualizer.py):
// no PipeWire at all -- no connection, no capture streams. An active slot's
// analysis window is filled with sine tones the test picks per slot and
// channel (visualizer_test_set_tone()) instead of captured audio; everything
// after that (slot on/off, FFT, smoothing, drawing) is the real path. So the
// test never touches the user's audio system.
static bool g_synth = false;

// The thread loop's lock, or nothing in synth mode (there's no thread loop).
static void lock(void) {
    if (g_loop) pw_thread_loop_lock(g_loop);
}
static void unlock(void) {
    if (g_loop) pw_thread_loop_unlock(g_loop);
}

// --- PipeWire callbacks (thread loop's own thread, lock held) ---

static void on_process(void *data) {
    Source *s = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
    if (!b) {
        return;
    }
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk) {
        uint32_t offset = d->chunk->offset < d->maxsize ? d->chunk->offset : d->maxsize;
        uint32_t size = d->chunk->size;
        if (size > d->maxsize - offset) {
            size = d->maxsize - offset;
        }
        const float *samples = (const float *)((const uint8_t *)d->data + offset);
        uint32_t frames = size / (sizeof(float) * VISUALIZER_CHANNELS);
        for (uint32_t i = 0; i < frames; i++) {
            s->ring[VISUALIZER_LEFT][s->pos] = samples[2 * i];
            s->ring[VISUALIZER_RIGHT][s->pos] = samples[2 * i + 1];
            s->pos = (s->pos + 1) % SPECTRUM_FFT_SIZE;
        }
        s->fresh += frames;
    }
    pw_stream_queue_buffer(s->stream, b);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    Source *s = data;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "stickBar: visualizer capture stream %d error: %s\n", s->index, error ? error : "?");
    }
}

static const struct pw_stream_events g_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

// --- Timer ---

static void set_timer(bool armed) {
    if (armed == g_timer_armed || g_timer_fd < 0) {
        return;
    }
    struct itimerspec spec = {0}; // all zero = disarm
    if (armed) {
        spec.it_interval.tv_nsec = FRAME_NS;
        spec.it_value.tv_nsec = FRAME_NS;
    }
    if (timerfd_settime(g_timer_fd, 0, &spec, NULL) < 0) {
        perror("stickBar: timerfd_settime(visualizer)");
        return;
    }
    g_timer_armed = armed;
}

// --- Streams (call with the thread loop locked) ---

static void destroy_stream(Source *s) {
    if (s->stream) {
        spa_hook_remove(&s->listener);
        pw_stream_destroy(s->stream);
        s->stream = NULL;
    }
}

// Creates and connects `s`'s capture stream for `target`, initially active.
static bool create_stream(Source *s, uint32_t target) {
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_NODE_NAME, VISUALIZER_NODE_NAME,
        PW_KEY_NODE_DESCRIPTION, "stickBar media visualizer",
        // Don't keep anything awake on our account -- if nothing else is
        // playing, there'd be nothing to show anyway.
        PW_KEY_NODE_PASSIVE, "true",
        // ~21ms per buffer. Plenty for a 15fps display; the graph's own
        // quantum caps how big buffers can actually get anyway.
        PW_KEY_NODE_LATENCY, "1024/48000",
        NULL);
    if (!props) {
        return false;
    }
    if (target == VISUALIZER_TARGET_MIX) {
        // Capture the *sink's* monitor rather than a source: with no target
        // set, the session manager links this to the default sink and moves
        // it along when the default changes (headphones/DAC switches).
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        // Test-only: tests/test_visualizer.py may point the mix at a
        // throwaway null sink, so it never reads what the user is playing.
        const char *test_target = getenv("STICKBAR_TEST_VISUALIZER_TARGET");
        if (test_target && test_target[0]) {
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, test_target);
        }
    } else {
        // One app's playback stream: link to its output ports directly. And
        // never anything else -- without dont-fallback, a target that's gone
        // (the app closed the stream between tracks) makes the session
        // manager link a capture stream to the default *source*, i.e. the
        // microphone. A gone target instead leaves this stream unlinked
        // (silent) until media_popup re-resolves the player's stream.
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%u", target);
        pw_properties_set(props, "node.dont-fallback", "true");
        pw_properties_set(props, "node.dont-reconnect", "true");
    }

    s->stream = pw_stream_new(g_core, VISUALIZER_NODE_NAME, props);
    if (!s->stream) {
        return false;
    }
    pw_stream_add_listener(s->stream, &s->listener, &g_stream_events, s);

    uint8_t pod_buf[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32,
                                 .channels = 2,
                                 .rate = CAPTURE_RATE,
                                 .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));
    int res = pw_stream_connect(s->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    if (res < 0) {
        fprintf(stderr, "stickBar: pw_stream_connect(visualizer %d) failed: %s\n", s->index, strerror(-res));
        destroy_stream(s);
        return false;
    }
    s->target = target;
    return true;
}

// --- Public API ---

bool visualizer_init(void) {
    // Test-only: tests/lib/common.py sets this for every test bar except
    // test_visualizer.py's -- see NO_VISUALIZER there.
    const char *disabled = getenv("STICKBAR_TEST_NO_VISUALIZER");
    if (disabled && disabled[0]) {
        return false;
    }

    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        g_sources[i].index = i;
        g_sources[i].settled = true;
    }

    g_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (g_timer_fd < 0) {
        perror("stickBar: timerfd_create(visualizer)");
        return false;
    }

    const char *synth = getenv("STICKBAR_TEST_VISUALIZER_SYNTH");
    if (synth && synth[0]) {
        g_synth = true;
        g_available = true;
        return true;
    }

    pw_init(NULL, NULL); // refcounted -- pipewire_client.c calls it too
    g_loop = pw_thread_loop_new("stickBar-visualizer", NULL);
    g_context = g_loop ? pw_context_new(pw_thread_loop_get_loop(g_loop), NULL, 0) : NULL;
    if (!g_context || pw_thread_loop_start(g_loop) < 0) {
        fprintf(stderr, "stickBar: couldn't set up the visualizer's PipeWire thread -- disabled for this run\n");
        visualizer_shutdown();
        return false;
    }
    // One connection for every slot's stream (pw_stream_new_simple() would
    // open a separate one per stream).
    pw_thread_loop_lock(g_loop);
    g_core = pw_context_connect(g_context, NULL, 0);
    pw_thread_loop_unlock(g_loop);
    if (!g_core) {
        fprintf(stderr, "stickBar: visualizer couldn't connect to PipeWire -- disabled for this run\n");
        visualizer_shutdown();
        return false;
    }

    // No streams yet: each slot's is created the first time it's turned on,
    // so a bar whose media dropdown is never opened never captures anything.
    g_available = true;
    return true;
}

void visualizer_shutdown(void) {
    if (g_loop) {
        pw_thread_loop_lock(g_loop);
        for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
            destroy_stream(&g_sources[i]);
        }
        if (g_core) {
            pw_core_disconnect(g_core);
            g_core = NULL;
        }
        pw_thread_loop_unlock(g_loop);
        pw_thread_loop_stop(g_loop); // no-op if never started
    }
    if (g_context) {
        pw_context_destroy(g_context);
        g_context = NULL;
    }
    if (g_loop) {
        pw_thread_loop_destroy(g_loop);
        g_loop = NULL;
    }
    if (g_timer_fd >= 0) {
        close(g_timer_fd);
        g_timer_fd = -1;
    }
    g_available = false;
    g_timer_armed = false;
    g_synth = false;
    memset(g_sources, 0, sizeof(g_sources));
}

void visualizer_test_set_tone(int slot, int channel, double hz) {
    if (slot >= 0 && slot < VISUALIZER_MAX_SOURCES && channel >= 0 && channel < VISUALIZER_CHANNELS) {
        g_sources[slot].synth_hz[channel] = hz;
    }
}

bool visualizer_available(void) {
    return g_available;
}

int visualizer_timer_fd(void) {
    return g_timer_fd;
}

const float *visualizer_levels(int slot, int channel) {
    return g_sources[slot].levels[channel];
}

const float *visualizer_peaks(int slot, int channel) {
    return g_sources[slot].peaks[channel];
}

void visualizer_set_source(int slot, uint32_t target_serial, bool active) {
    if (!g_available || slot < 0 || slot >= VISUALIZER_MAX_SOURCES) {
        return;
    }
    Source *s = &g_sources[slot];
    if (g_synth) {
        // No stream to manage: the slot's on/off state is all there is.
        if (active != s->active) {
            s->active = active;
            if (active) {
                s->settled = false;
                set_timer(true);
            }
        }
        return;
    }
    bool retarget = active && s->stream && s->target != target_serial;
    if (active == s->active && !retarget) {
        return;
    }

    pw_thread_loop_lock(g_loop);
    if (active) {
        if (retarget) {
            destroy_stream(s);
        }
        if (!s->stream) {
            create_stream(s, target_serial); // connects active
        } else {
            pw_stream_set_active(s->stream, true);
        }
    } else if (s->stream) {
        pw_stream_set_active(s->stream, false);
    }
    // Either way, the old window is no longer this slot's current audio:
    // without new samples it would stay frozen in the rings, and the graph
    // would hold its pose -- silence lets it fall back to rest (or start
    // from rest on a new target).
    memset(s->ring, 0, sizeof(s->ring));
    s->fresh = 0;
    pw_thread_loop_unlock(g_loop);

    s->active = active && s->stream;
    if (s->active) {
        s->settled = false;
        set_timer(true);
    }
    // Turned off: the timer keeps running until visualizer_pump() sees every
    // slot has settled, then disarms itself.
}

// Moves `levels` toward `raw` by one frame of smoothing (see ATTACK and
// FALL_PER_FRAME), and the peak markers after them. Reports whether anything
// moved, and whether everything is at rest (0).
static void smooth(float *levels, float *peaks, int *hold, const float *raw, bool *changed, bool *settled) {
    for (int i = 0; i < SPECTRUM_BAR_COUNT; i++) {
        float cur = levels[i];
        float next;
        if (raw[i] > cur) {
            next = cur + (raw[i] - cur) * ATTACK;
        } else {
            next = cur - FALL_PER_FRAME;
            if (next < raw[i]) {
                next = raw[i];
            }
        }
        levels[i] = next;

        float old_peak = peaks[i];
        if (next >= peaks[i]) {
            peaks[i] = next;
            hold[i] = PEAK_HOLD_FRAMES;
        } else if (hold[i] > 0) {
            hold[i]--;
        } else {
            peaks[i] -= PEAK_FALL_PER_FRAME;
            if (peaks[i] < next) {
                peaks[i] = next;
            }
        }

        if (fabsf(next - cur) > 1e-4f || fabsf(peaks[i] - old_peak) > 1e-4f) {
            *changed = true;
        }
        if (next > 0.0f || peaks[i] > 0.0f) {
            *settled = false;
        }
    }
}

// One frame of analysis for one slot: both channels' windows out of the rings
// (under the lock), FFTs and smoothing outside it so the capture thread is
// never held up. Returns whether anything moved.
static bool analyse(Source *s) {
    static float window[VISUALIZER_CHANNELS][SPECTRUM_FFT_SIZE];
    if (g_synth) {
        // Synthetic audio while active, silence once turned off (as a paused
        // real stream gives).
        for (int ch = 0; ch < VISUALIZER_CHANNELS; ch++) {
            double hz = s->active ? s->synth_hz[ch] : 0.0;
            for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
                window[ch][i] = hz > 0.0 ? 0.5f * (float)sin(2.0 * M_PI * hz * i / CAPTURE_RATE) : 0.0f;
            }
        }
        goto analyse_window;
    }
    lock();
    s->stale_frames = s->fresh ? 0 : s->stale_frames + 1;
    s->fresh = 0;
    if (s->stale_frames >= STALE_FRAMES) {
        memset(s->ring, 0, sizeof(s->ring)); // see STALE_FRAMES
    }
    unsigned pos = s->pos;
    for (int ch = 0; ch < VISUALIZER_CHANNELS; ch++) {
        memcpy(window[ch], s->ring[ch] + pos, (SPECTRUM_FFT_SIZE - pos) * sizeof(float));
        memcpy(window[ch] + (SPECTRUM_FFT_SIZE - pos), s->ring[ch], pos * sizeof(float));
    }
    unlock();

analyse_window:;
    bool changed = false, settled = true;
    for (int ch = 0; ch < VISUALIZER_CHANNELS; ch++) {
        float raw[SPECTRUM_BAR_COUNT];
        spectrum_analyze(window[ch], (float)CAPTURE_RATE);
        spectrum_bands(SPECTRUM_BAR_COUNT, raw);
        smooth(s->levels[ch], s->peaks[ch], s->peak_hold[ch], raw, &changed, &settled);
    }
    s->settled = settled;
    return changed;
}

uint32_t visualizer_pump(uint32_t ready_mask, uint32_t shown_mask) {
    if (!g_available) {
        return 0;
    }
    uint64_t expirations;
    // Drain the timerfd; how many frames were missed doesn't matter.
    (void)!read(g_timer_fd, &expirations, sizeof(expirations));

    uint32_t changed = 0;
    bool busy = false;
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        Source *s = &g_sources[i];
        uint32_t bit = 1u << i;
        if (!s->active && s->settled) {
            continue;
        }
        if (!s->active && !(shown_mask & bit)) {
            // Off and not on screen: nothing will ever ask for its frames, so
            // drop it to rest at once instead of decaying levels nobody sees.
            memset(s->levels, 0, sizeof(s->levels));
            memset(s->peaks, 0, sizeof(s->peaks));
            s->settled = true;
            continue;
        }
        // Not ready (the compositor hasn't shown its last frame yet): no
        // analysis this tick -- it would only be thrown away.
        if ((ready_mask & bit) && analyse(s)) {
            changed |= bit;
        }
        if (s->active || !s->settled) {
            busy = true;
        }
    }
    if (!busy) {
        set_timer(false);
    }
    return changed;
}
