#include "audio/pulse_client.h"

#include <math.h>
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

// Fixed-size, matches this project's existing convention (sni_registry.c's
// MAX_TRAY_ITEMS) of a bounded array over dynamic growth -- generous for any
// real machine's sink/source count. Slots aren't moved/reordered once handed
// out, same reasoning as SniItem: a PulseNode* returned to a caller (the
// device popup) stays valid until that device is removed.
#define MAX_PULSE_NODES 32

struct PulseNode {
    bool in_use;
    bool is_sink; // true = sink (speaker/headset output), false = source (mic)
    uint32_t index;
    uint8_t channels;
    char name[256];        // stable device name, e.g. "alsa_output.usb-..." --
                            // this, not `index`, is what pa_server_info's
                            // default_sink_name/default_source_name match against
    char description[256]; // human-readable, e.g. "Yeti X Analog Stereo"
    int volume_percent;
    bool muted;
};

static pa_threaded_mainloop *g_mainloop = NULL;
static pa_context *g_context = NULL;
static int g_notify_fd = -1;

// Per-application playback streams ("sink inputs" in PA terms -- one per app
// currently holding an output stream open, e.g. Firefox, Spotify, mpv), for
// audio/mixer_popup.c's per-app volume mixer. Same fixed-array, never-
// relocated-slot convention as PulseNode above, for the same reason: the
// mixer popup holds PulseStream* across redraws.
#define MAX_PULSE_STREAMS 32

struct PulseStream {
    bool in_use;
    uint32_t index;
    uint8_t channels;
    bool has_volume;  // false for e.g. passthrough streams, which can't be
                      // volume-adjusted at all -- the mixer still lists them,
                      // but setting their volume is silently skipped
    char name[256];   // application.name, falling back to the stream's own name
    int volume_percent;
    bool muted;
};

static struct PulseNode g_sinks[MAX_PULSE_NODES];
static struct PulseNode g_sources[MAX_PULSE_NODES];
static char g_default_sink_name[256] = "";
static char g_default_source_name[256] = "";

// Wakes up the main epoll loop to re-read the cache and redraw. Safe to call
// from inside a PA callback (always running on the mainloop's own thread, with
// its lock already held per pa_threaded_mainloop's documented model) --
// eventfd_write() doesn't touch any of libpulse's own data structures, so no
// separate synchronization is needed for this part.
static void notify_dirty(void) {
    if (g_notify_fd >= 0) {
        eventfd_write(g_notify_fd, 1);
    }
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// --- Cache mutation, called only from PA callbacks (mainloop thread, lock
// already held) ---

static struct PulseNode *find_node_by_index(bool is_sink, uint32_t index) {
    struct PulseNode *arr = is_sink ? g_sinks : g_sources;
    for (int i = 0; i < MAX_PULSE_NODES; i++) {
        if (arr[i].in_use && arr[i].index == index) {
            return &arr[i];
        }
    }
    return NULL;
}

static struct PulseNode *find_free_slot(bool is_sink) {
    struct PulseNode *arr = is_sink ? g_sinks : g_sources;
    for (int i = 0; i < MAX_PULSE_NODES; i++) {
        if (!arr[i].in_use) {
            return &arr[i];
        }
    }
    return NULL;
}

static struct PulseNode *find_node_by_name(bool is_sink, const char *name) {
    if (!name || !name[0]) {
        return NULL;
    }
    struct PulseNode *arr = is_sink ? g_sinks : g_sources;
    for (int i = 0; i < MAX_PULSE_NODES; i++) {
        if (arr[i].in_use && strcmp(arr[i].name, name) == 0) {
            return &arr[i];
        }
    }
    return NULL;
}

static void upsert_node(bool is_sink, uint32_t index, const char *name, const char *description,
                         const pa_cvolume *volume, int mute) {
    struct PulseNode *node = find_node_by_index(is_sink, index);
    if (!node) {
        node = find_free_slot(is_sink);
        if (!node) {
            fprintf(stderr, "stickBar: pulse %s limit (%d) reached, ignoring index %u\n",
                    is_sink ? "sink" : "source", MAX_PULSE_NODES, index);
            return;
        }
        memset(node, 0, sizeof(*node));
        node->in_use = true;
        node->is_sink = is_sink;
        node->index = index;
    }
    snprintf(node->name, sizeof(node->name), "%s", name ? name : "");
    snprintf(node->description, sizeof(node->description), "%s", description ? description : (name ? name : ""));
    node->channels = volume->channels;
    node->volume_percent = (int)lround((double)pa_cvolume_avg(volume) * 100.0 / PA_VOLUME_NORM);
    node->muted = mute != 0;
}

static void remove_node(bool is_sink, uint32_t index) {
    struct PulseNode *node = find_node_by_index(is_sink, index);
    if (node) {
        memset(node, 0, sizeof(*node)); // in_use = false
    }
}

static struct PulseStream g_streams[MAX_PULSE_STREAMS];

static struct PulseStream *find_stream_by_index(uint32_t index) {
    for (int i = 0; i < MAX_PULSE_STREAMS; i++) {
        if (g_streams[i].in_use && g_streams[i].index == index) {
            return &g_streams[i];
        }
    }
    return NULL;
}

static void remove_stream(uint32_t index) {
    struct PulseStream *stream = find_stream_by_index(index);
    if (stream) {
        memset(stream, 0, sizeof(*stream)); // in_use = false
    }
}

// --- PA callbacks (mainloop thread) ---

static void sink_info_cb(pa_context *ctx, const pa_sink_info *info, int eol, void *userdata) {
    (void)ctx; (void)userdata;
    if (eol || !info) {
        return; // eol=1 marks the end of a get_sink_info_list() batch; info is NULL then
    }
    upsert_node(true, info->index, info->name, info->description, &info->volume, info->mute);
    notify_dirty();
}

static void source_info_cb(pa_context *ctx, const pa_source_info *info, int eol, void *userdata) {
    (void)ctx; (void)userdata;
    if (eol || !info) {
        return;
    }
    upsert_node(false, info->index, info->name, info->description, &info->volume, info->mute);
    notify_dirty();
}

static void sink_input_info_cb(pa_context *ctx, const pa_sink_input_info *info, int eol, void *userdata) {
    (void)ctx; (void)userdata;
    if (eol || !info) {
        return;
    }
    struct PulseStream *stream = find_stream_by_index(info->index);
    if (!stream) {
        for (int i = 0; i < MAX_PULSE_STREAMS; i++) {
            if (!g_streams[i].in_use) {
                stream = &g_streams[i];
                break;
            }
        }
        if (!stream) {
            fprintf(stderr, "stickBar: pulse stream limit (%d) reached, ignoring index %u\n",
                    MAX_PULSE_STREAMS, info->index);
            return;
        }
        memset(stream, 0, sizeof(*stream));
        stream->in_use = true;
        stream->index = info->index;
    }

    // application.name is what pavucontrol shows as each row's title too
    // (e.g. "Firefox", "spotify") -- info->name is the stream's own name
    // (often something generic like "AudioStream" or "Playback"), only used
    // as a fallback for clients that don't set application.name at all.
    const char *app_name = info->proplist ? pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME) : NULL;
    snprintf(stream->name, sizeof(stream->name), "%s",
             app_name && app_name[0] ? app_name : (info->name ? info->name : "Unknown app"));
    stream->has_volume = info->has_volume != 0;
    stream->channels = info->volume.channels;
    stream->volume_percent = (int)lround((double)pa_cvolume_avg(&info->volume) * 100.0 / PA_VOLUME_NORM);
    stream->muted = info->mute != 0;
    notify_dirty();
}

static void server_info_cb(pa_context *ctx, const pa_server_info *info, void *userdata) {
    (void)ctx; (void)userdata;
    if (!info) {
        return;
    }
    snprintf(g_default_sink_name, sizeof(g_default_sink_name), "%s",
              info->default_sink_name ? info->default_sink_name : "");
    snprintf(g_default_source_name, sizeof(g_default_source_name), "%s",
              info->default_source_name ? info->default_source_name : "");
    notify_dirty();
}

static void subscribe_cb(pa_context *ctx, pa_subscription_event_type_t type, uint32_t idx, void *userdata) {
    (void)userdata;
    unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    unsigned kind = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    switch (facility) {
    case PA_SUBSCRIPTION_EVENT_SINK:
        if (kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            remove_node(true, idx);
            notify_dirty();
        } else {
            // Re-fetch just this one sink rather than the whole list -- same
            // "single-item refresh on signal" pattern sni_registry.c's
            // refresh_icon() uses for NewIcon.
            pa_context_get_sink_info_by_index(ctx, idx, sink_info_cb, NULL);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
        if (kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            remove_node(false, idx);
            notify_dirty();
        } else {
            pa_context_get_source_info_by_index(ctx, idx, source_info_cb, NULL);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
        if (kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
            remove_stream(idx);
            notify_dirty();
        } else {
            pa_context_get_sink_input_info(ctx, idx, sink_input_info_cb, NULL);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SERVER:
        // Default sink/source changed -- re-fetch server info for the new names.
        pa_context_get_server_info(ctx, server_info_cb, NULL);
        break;
    default:
        break; // cards, modules, clients, etc. -- not tracked
    }
}

static void context_state_cb(pa_context *ctx, void *userdata) {
    (void)userdata;
    pa_context_state_t state = pa_context_get_state(ctx);
    switch (state) {
    case PA_CONTEXT_READY:
        pa_context_set_subscribe_callback(ctx, subscribe_cb, NULL);
        pa_context_subscribe(ctx, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
                                       PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SERVER,
                             NULL, NULL);
        pa_context_get_sink_info_list(ctx, sink_info_cb, NULL);
        pa_context_get_sink_input_info_list(ctx, sink_input_info_cb, NULL);
        pa_context_get_source_info_list(ctx, source_info_cb, NULL);
        pa_context_get_server_info(ctx, server_info_cb, NULL);
        pa_threaded_mainloop_signal(g_mainloop, 0);
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        // No reconnect attempt in v1 -- matches this project's existing
        // precedent for the Hyprland event socket (one connection attempt,
        // degrade gracefully rather than retry forever). Wakes up
        // pulse_client_init()'s wait loop if the failure happened before the
        // very first PA_CONTEXT_READY.
        pa_threaded_mainloop_signal(g_mainloop, 0);
        break;
    default:
        break;
    }
}

// --- Public API ---

bool pulse_client_init(void) {
    g_mainloop = pa_threaded_mainloop_new();
    if (!g_mainloop) {
        fprintf(stderr, "stickBar: pa_threaded_mainloop_new() failed -- "
                         "volume/mic controls disabled for this run\n");
        return false;
    }

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(g_mainloop);
    g_context = pa_context_new(api, "stickBar");
    if (!g_context) {
        pa_threaded_mainloop_free(g_mainloop);
        g_mainloop = NULL;
        return false;
    }

    g_notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_notify_fd < 0) {
        perror("stickBar: eventfd(pulse notify)");
        pa_context_unref(g_context);
        g_context = NULL;
        pa_threaded_mainloop_free(g_mainloop);
        g_mainloop = NULL;
        return false;
    }

    pa_context_set_state_callback(g_context, context_state_cb, NULL);

    if (pa_context_connect(g_context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0 ||
        pa_threaded_mainloop_start(g_mainloop) < 0) {
        fprintf(stderr, "stickBar: failed to start the PulseAudio client thread -- "
                         "volume/mic controls disabled for this run\n");
        pulse_client_shutdown();
        return false;
    }

    // Block (this is startup, before the epoll loop begins -- same tradeoff
    // main.c already makes for the two Wayland roundtrips in wayland_connect())
    // until the context is definitively ready or definitively not, so
    // pulse_client_init()'s return value is immediately trustworthy instead of
    // the caller finding out several seconds later via a redraw that never
    // comes. This lock/wait/unlock dance is libpulse's own documented pattern
    // for synchronizing the owning thread against a pa_threaded_mainloop.
    pa_threaded_mainloop_lock(g_mainloop);
    while (true) {
        pa_context_state_t state = pa_context_get_state(g_context);
        if (state == PA_CONTEXT_READY || state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            break;
        }
        pa_threaded_mainloop_wait(g_mainloop);
    }
    bool ready = pa_context_get_state(g_context) == PA_CONTEXT_READY;
    pa_threaded_mainloop_unlock(g_mainloop);

    if (!ready) {
        fprintf(stderr, "stickBar: failed to connect to the PulseAudio-compatible "
                         "server -- volume/mic controls disabled for this run\n");
        pulse_client_shutdown();
        return false;
    }
    return true;
}

void pulse_client_shutdown(void) {
    if (g_mainloop && g_context) {
        pa_threaded_mainloop_lock(g_mainloop);
        pa_context_disconnect(g_context);
        pa_context_unref(g_context);
        pa_threaded_mainloop_unlock(g_mainloop);
    } else if (g_context) {
        pa_context_unref(g_context);
    }
    g_context = NULL;

    if (g_mainloop) {
        pa_threaded_mainloop_stop(g_mainloop);
        pa_threaded_mainloop_free(g_mainloop);
        g_mainloop = NULL;
    }

    if (g_notify_fd >= 0) {
        close(g_notify_fd);
        g_notify_fd = -1;
    }
}

int pulse_client_notify_fd(void) {
    return g_notify_fd;
}

void pulse_client_pump(void) {
    if (g_notify_fd < 0) {
        return;
    }
    eventfd_t val;
    // A single read fully drains the counter (eventfd's default, non-semaphore
    // mode reads-and-resets the whole accumulated value at once) -- any number
    // of notify_dirty() calls since the last pump collapse into one redraw.
    (void)eventfd_read(g_notify_fd, &val);
}

int pulse_default_sink_volume_percent(void) {
    if (!g_mainloop) return 0;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(true, g_default_sink_name);
    int pct = n ? n->volume_percent : 0;
    pa_threaded_mainloop_unlock(g_mainloop);
    return pct;
}

bool pulse_default_sink_muted(void) {
    if (!g_mainloop) return false;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(true, g_default_sink_name);
    bool muted = n ? n->muted : false;
    pa_threaded_mainloop_unlock(g_mainloop);
    return muted;
}

void pulse_default_sink_adjust_volume(int delta_percent) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(true, g_default_sink_name);
    if (n) {
        int new_pct = clampi(n->volume_percent + delta_percent, 0, PULSE_MAX_VOLUME_PERCENT);
        pa_cvolume cv;
        pa_cvolume_set(&cv, n->channels > 0 ? n->channels : 2,
                       (pa_volume_t)lround((double)new_pct / 100.0 * PA_VOLUME_NORM));
        // Fire-and-forget: the eventual SINK subscribe event (triggered by the
        // server actually applying this change) is what updates the cache and
        // redraws, not this call's own (unused) success callback -- same
        // reasoning as sni_item_activate()'s fire-and-forget Activate() call.
        pa_context_set_sink_volume_by_index(g_context, n->index, &cv, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}

void pulse_default_sink_toggle_mute(void) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(true, g_default_sink_name);
    if (n) {
        pa_context_set_sink_mute_by_index(g_context, n->index, !n->muted, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}

int pulse_default_source_volume_percent(void) {
    if (!g_mainloop) return 0;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(false, g_default_source_name);
    int pct = n ? n->volume_percent : 0;
    pa_threaded_mainloop_unlock(g_mainloop);
    return pct;
}

bool pulse_default_source_muted(void) {
    if (!g_mainloop) return false;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(false, g_default_source_name);
    bool muted = n ? n->muted : false;
    pa_threaded_mainloop_unlock(g_mainloop);
    return muted;
}

void pulse_default_source_adjust_volume(int delta_percent) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(false, g_default_source_name);
    if (n) {
        int new_pct = clampi(n->volume_percent + delta_percent, 0, PULSE_MAX_VOLUME_PERCENT);
        pa_cvolume cv;
        pa_cvolume_set(&cv, n->channels > 0 ? n->channels : 2,
                       (pa_volume_t)lround((double)new_pct / 100.0 * PA_VOLUME_NORM));
        pa_context_set_source_volume_by_index(g_context, n->index, &cv, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}

void pulse_default_source_toggle_mute(void) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    struct PulseNode *n = find_node_by_name(false, g_default_source_name);
    if (n) {
        pa_context_set_source_mute_by_index(g_context, n->index, !n->muted, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}

int pulse_sink_count(void) {
    if (!g_mainloop) return 0;
    pa_threaded_mainloop_lock(g_mainloop);
    int count = 0;
    for (int i = 0; i < MAX_PULSE_NODES; i++) {
        if (g_sinks[i].in_use) count++;
    }
    pa_threaded_mainloop_unlock(g_mainloop);
    return count;
}

PulseNode *pulse_sink_get(int index) {
    if (!g_mainloop) return NULL;
    pa_threaded_mainloop_lock(g_mainloop);
    int seen = 0;
    PulseNode *result = NULL;
    for (int i = 0; i < MAX_PULSE_NODES; i++) {
        if (g_sinks[i].in_use) {
            if (seen == index) { result = &g_sinks[i]; break; }
            seen++;
        }
    }
    pa_threaded_mainloop_unlock(g_mainloop);
    return result;
}

int pulse_source_count(void) {
    if (!g_mainloop) return 0;
    pa_threaded_mainloop_lock(g_mainloop);
    int count = 0;
    for (int i = 0; i < MAX_PULSE_NODES; i++) {
        if (g_sources[i].in_use) count++;
    }
    pa_threaded_mainloop_unlock(g_mainloop);
    return count;
}

PulseNode *pulse_source_get(int index) {
    if (!g_mainloop) return NULL;
    pa_threaded_mainloop_lock(g_mainloop);
    int seen = 0;
    PulseNode *result = NULL;
    for (int i = 0; i < MAX_PULSE_NODES; i++) {
        if (g_sources[i].in_use) {
            if (seen == index) { result = &g_sources[i]; break; }
            seen++;
        }
    }
    pa_threaded_mainloop_unlock(g_mainloop);
    return result;
}

const char *pulse_node_description(PulseNode *node) {
    // Safe to read without locking: `description` is only ever written once
    // per upsert as a complete snprintf() into a fixed buffer already sized
    // for the max PA description length seen in practice, and callers here
    // only ever display it, never mutate it -- worst case under a genuine
    // concurrent update is stale-but-still-valid text for one frame, matching
    // this project's overall tolerance for eventually-consistent redraws
    // (e.g. tray.c reads sni_registry state the same unlocked way).
    return node->description;
}

int pulse_node_volume_percent(PulseNode *node) {
    if (!g_mainloop) return 0;
    pa_threaded_mainloop_lock(g_mainloop);
    int pct = node->volume_percent;
    pa_threaded_mainloop_unlock(g_mainloop);
    return pct;
}

bool pulse_node_muted(PulseNode *node) {
    if (!g_mainloop) return false;
    pa_threaded_mainloop_lock(g_mainloop);
    bool muted = node->muted;
    pa_threaded_mainloop_unlock(g_mainloop);
    return muted;
}

bool pulse_node_is_default(PulseNode *node) {
    if (!g_mainloop) return false;
    pa_threaded_mainloop_lock(g_mainloop);
    const char *default_name = node->is_sink ? g_default_sink_name : g_default_source_name;
    bool is_default = default_name[0] != '\0' && strcmp(node->name, default_name) == 0;
    pa_threaded_mainloop_unlock(g_mainloop);
    return is_default;
}

void pulse_node_set_default(PulseNode *node) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    if (node->is_sink) {
        pa_context_set_default_sink(g_context, node->name, NULL, NULL);
    } else {
        pa_context_set_default_source(g_context, node->name, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}

void pulse_node_toggle_mute(PulseNode *node) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    if (node->is_sink) {
        pa_context_set_sink_mute_by_index(g_context, node->index, !node->muted, NULL, NULL);
    } else {
        pa_context_set_source_mute_by_index(g_context, node->index, !node->muted, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}

int pulse_stream_count(void) {
    if (!g_mainloop) return 0;
    pa_threaded_mainloop_lock(g_mainloop);
    int count = 0;
    for (int i = 0; i < MAX_PULSE_STREAMS; i++) {
        if (g_streams[i].in_use) count++;
    }
    pa_threaded_mainloop_unlock(g_mainloop);
    return count;
}

PulseStream *pulse_stream_get(int index) {
    if (!g_mainloop) return NULL;
    pa_threaded_mainloop_lock(g_mainloop);
    int seen = 0;
    PulseStream *result = NULL;
    for (int i = 0; i < MAX_PULSE_STREAMS; i++) {
        if (g_streams[i].in_use) {
            if (seen == index) { result = &g_streams[i]; break; }
            seen++;
        }
    }
    pa_threaded_mainloop_unlock(g_mainloop);
    return result;
}

const char *pulse_stream_name(PulseStream *stream) {
    // Unlocked, same reasoning as pulse_node_description() above.
    return stream->name;
}

int pulse_stream_volume_percent(PulseStream *stream) {
    if (!g_mainloop) return 0;
    pa_threaded_mainloop_lock(g_mainloop);
    int pct = stream->volume_percent;
    pa_threaded_mainloop_unlock(g_mainloop);
    return pct;
}

bool pulse_stream_muted(PulseStream *stream) {
    if (!g_mainloop) return false;
    pa_threaded_mainloop_lock(g_mainloop);
    bool muted = stream->muted;
    pa_threaded_mainloop_unlock(g_mainloop);
    return muted;
}

void pulse_stream_set_volume_percent(PulseStream *stream, int percent) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    // The in_use check matters here, unlike the read-only accessors above: a
    // caller can still be holding this pointer after the app closed its
    // stream (e.g. mid-drag in the mixer popup), and a freed slot's zeroed
    // `index` of 0 is a perfectly valid *other* stream's index on the server.
    if (stream->in_use && stream->has_volume) {
        int new_pct = clampi(percent, 0, PULSE_MAX_VOLUME_PERCENT);
        pa_cvolume cv;
        pa_cvolume_set(&cv, stream->channels > 0 ? stream->channels : 2,
                       (pa_volume_t)lround((double)new_pct / 100.0 * PA_VOLUME_NORM));
        // Fire-and-forget, same as pulse_default_sink_adjust_volume().
        pa_context_set_sink_input_volume(g_context, stream->index, &cv, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}

void pulse_stream_adjust_volume(PulseStream *stream, int delta_percent) {
    // Reads the cached percent under the lock first, then reuses the setter
    // (which re-takes the lock) -- the lock isn't recursive, so the two
    // can't nest.
    pulse_stream_set_volume_percent(stream, pulse_stream_volume_percent(stream) + delta_percent);
}

void pulse_stream_toggle_mute(PulseStream *stream) {
    if (!g_mainloop || !g_context) return;
    pa_threaded_mainloop_lock(g_mainloop);
    if (stream->in_use) {
        pa_context_set_sink_input_mute(g_context, stream->index, !stream->muted, NULL, NULL);
    }
    pa_threaded_mainloop_unlock(g_mainloop);
}
