#include "pipewire/pipewire_client.h"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

// Fixed-size, matching this project's existing convention (audio/pulse_client.c's
// MAX_PULSE_NODES, dbus/sni_registry.c's MAX_TRAY_ITEMS) of a bounded array over
// dynamic growth. A real desktop's PipeWire graph (audio devices, sinks/sources,
// playback/capture streams, filters) commonly runs in the 50-150 node range;
// 256 is generous headroom for both tables without being unbounded.
#define MAX_PW_NODES 256
#define MAX_PW_LINKS 256

typedef struct {
    bool in_use;
    uint32_t id; // registry global id
    char media_class[64];
    char display_name[128];

    // Only meaningful when is_tracked_stream is true (Stream/Input/Audio or
    // Stream/Input/Video) -- every other node (sinks, sources, filters, ...)
    // is still tracked (its media_class is needed as a possible *link
    // source* for classify-webcam-vs-screenshare below) but never bound, so
    // `running`/`proxy` stay at their zeroed defaults for it.
    bool is_tracked_stream;
    bool running;
    struct pw_proxy *proxy;
    struct spa_hook proxy_listener;
} PwNode;

typedef struct {
    bool in_use;
    uint32_t id; // the link's own global id, needed to match global_remove
    uint32_t output_node_id;
    uint32_t input_node_id;
} PwLink;

static struct pw_thread_loop *g_loop = NULL;
static struct pw_context *g_context = NULL;
static struct pw_core *g_core = NULL;
static struct pw_registry *g_registry = NULL;
static struct spa_hook g_registry_listener;
static int g_notify_fd = -1;

static PwNode g_nodes[MAX_PW_NODES];
static PwLink g_links[MAX_PW_LINKS];

// Wakes up the main epoll loop to re-read the cache and redraw. Safe to call
// from inside a PipeWire callback -- these always run on the thread-loop's own
// thread, with the thread-loop's lock already held per its documented model
// (same reasoning audio/pulse_client.c's notify_dirty() gives for
// pa_threaded_mainloop) -- eventfd_write() doesn't touch any PipeWire state,
// so no extra synchronization is needed for this part.
static void notify_dirty(void) {
    if (g_notify_fd >= 0) {
        eventfd_write(g_notify_fd, 1);
    }
}

// --- Cache mutation, called only from the registry/node callbacks (thread-loop's
// own thread, lock already held) ---

static PwNode *find_node_by_id(uint32_t id) {
    for (int i = 0; i < MAX_PW_NODES; i++) {
        if (g_nodes[i].in_use && g_nodes[i].id == id) {
            return &g_nodes[i];
        }
    }
    return NULL;
}

static PwNode *find_free_node_slot(void) {
    for (int i = 0; i < MAX_PW_NODES; i++) {
        if (!g_nodes[i].in_use) {
            return &g_nodes[i];
        }
    }
    return NULL;
}

static PwLink *find_link_by_input_node(uint32_t input_node_id) {
    for (int i = 0; i < MAX_PW_LINKS; i++) {
        if (g_links[i].in_use && g_links[i].input_node_id == input_node_id) {
            return &g_links[i];
        }
    }
    return NULL;
}

static PwLink *find_free_link_slot(void) {
    for (int i = 0; i < MAX_PW_LINKS; i++) {
        if (!g_links[i].in_use) {
            return &g_links[i];
        }
    }
    return NULL;
}

// Fallback chain for a human-readable app name, tried in this order --
// application.name is what real apps set (confirmed live: a `parecord`
// capture during development showed application.name="parecord"), the rest
// are best-effort for streams that skip it.
static void resolve_display_name(const struct spa_dict *props, char *out, size_t out_len) {
    static const char *keys[] = {
        PW_KEY_APP_NAME,
        PW_KEY_NODE_DESCRIPTION,
        PW_KEY_NODE_NAME,
        PW_KEY_MEDIA_NAME,
    };
    if (props) {
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const char *v = spa_dict_lookup(props, keys[i]);
            if (v && v[0]) {
                snprintf(out, out_len, "%s", v);
                return;
            }
        }
    }
    snprintf(out, out_len, "Unknown app");
}

// --- PipeWire callbacks (thread-loop's own thread) ---

static void node_info_cb(void *data, const struct pw_node_info *info) {
    PwNode *node = (PwNode *)data;
    if (!info) {
        return;
    }
    bool was_running = node->running;
    node->running = info->state == PW_NODE_STATE_RUNNING;
    if (node->running != was_running) {
        notify_dirty();
    }
}

static const struct pw_node_events g_node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = node_info_cb,
};

static void handle_node_global(uint32_t id, const struct spa_dict *props) {
    const char *media_class = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : NULL;

    PwNode *node = find_free_node_slot();
    if (!node) {
        fprintf(stderr, "stickBar: pipewire node table limit (%d) reached, ignoring id %u\n",
                MAX_PW_NODES, id);
        return;
    }
    memset(node, 0, sizeof(*node));
    node->in_use = true;
    node->id = id;
    snprintf(node->media_class, sizeof(node->media_class), "%s", media_class ? media_class : "");
    resolve_display_name(props, node->display_name, sizeof(node->display_name));

    // Only Stream/Input/{Audio,Video} nodes need their live running state
    // watched -- everything else (sinks, real device source nodes used only
    // as a link's *source* for the webcam/screen-share classification below,
    // filters, ...) just needs its media_class cached, never bound.
    bool is_audio_input = strcmp(node->media_class, "Stream/Input/Audio") == 0;
    bool is_video_input = strcmp(node->media_class, "Stream/Input/Video") == 0;
    if (is_audio_input || is_video_input) {
        node->is_tracked_stream = true;
        struct pw_proxy *proxy = pw_registry_bind(g_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
        if (proxy) {
            node->proxy = proxy;
            pw_node_add_listener((struct pw_node *)proxy, &node->proxy_listener, &g_node_events, node);
        }
    }
    notify_dirty();
}

static void handle_link_global(uint32_t id, const struct spa_dict *props) {
    if (!props) {
        return;
    }
    const char *out_str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
    const char *in_str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
    if (!out_str || !in_str) {
        return;
    }
    PwLink *link = find_free_link_slot();
    if (!link) {
        fprintf(stderr, "stickBar: pipewire link table limit (%d) reached, ignoring id %u\n",
                MAX_PW_LINKS, id);
        return;
    }
    memset(link, 0, sizeof(*link));
    link->in_use = true;
    link->id = id;
    link->output_node_id = (uint32_t)strtoul(out_str, NULL, 10);
    link->input_node_id = (uint32_t)strtoul(in_str, NULL, 10);
    // A newly-appeared link can flip a Stream/Input/Video node's webcam-vs-
    // screenshare classification (see video_node_is_webcam() below), so this
    // needs to trigger a redraw the same as a node change would.
    notify_dirty();
}

static void registry_event_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                                   uint32_t version, const struct spa_dict *props) {
    (void)data;
    (void)permissions;
    (void)version;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        handle_node_global(id, props);
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        handle_link_global(id, props);
    }
}

static void registry_event_global_remove(void *data, uint32_t id) {
    (void)data;
    for (int i = 0; i < MAX_PW_NODES; i++) {
        if (g_nodes[i].in_use && g_nodes[i].id == id) {
            if (g_nodes[i].proxy) {
                pw_proxy_destroy(g_nodes[i].proxy);
            }
            memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
            notify_dirty();
            return;
        }
    }
    for (int i = 0; i < MAX_PW_LINKS; i++) {
        if (g_links[i].in_use && g_links[i].id == id) {
            memset(&g_links[i], 0, sizeof(g_links[i]));
            notify_dirty();
            return;
        }
    }
}

static const struct pw_registry_events g_registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

// --- Classification, called under the thread-loop lock by the accessors below ---

static bool node_is_active_stream(const PwNode *n) {
    return n->in_use && n->is_tracked_stream && n->running;
}

// Distinguishes a webcam consumer from a screen-share consumer for a
// Stream/Input/Video node, by following the PipeWire link feeding it back to
// its source node and checking *that* node's own media.class: a real camera
// device (v4l2/libcamera) is tagged "Video/Source" by PipeWire's own
// device-monitor convention -- confirmed live for the analogous microphone
// case during development, where real ALSA mic devices show up as
// "Audio/Source" nodes. A xdg-desktop-portal-created screen-capture producer
// is not a device-monitor node and doesn't carry that class, so "anything
// else" (including no link at all yet) falls into the screen-share bucket by
// elimination. NOT independently verified against real webcam hardware or a
// real portal screen-share session -- this development machine has neither
// (no /dev/video* device, and driving the portal's screen picker needs
// interactive UI a headless test can't provide) -- see CLAUDE.md's "Privacy
// indicator" section for what to re-check first if this is ever wrong on
// real hardware.
static bool video_node_is_webcam(const PwNode *n) {
    PwLink *link = find_link_by_input_node(n->id);
    if (!link) {
        return false;
    }
    PwNode *source = find_node_by_id(link->output_node_id);
    if (!source) {
        return false;
    }
    return strncmp(source->media_class, "Video/Source", strlen("Video/Source")) == 0;
}

// --- Public API ---

bool pipewire_client_init(void) {
    pw_init(NULL, NULL);

    g_notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_notify_fd < 0) {
        perror("stickBar: eventfd(pipewire notify)");
        return false;
    }

    g_loop = pw_thread_loop_new("stickBar-pipewire", NULL);
    if (!g_loop) {
        fprintf(stderr, "stickBar: pw_thread_loop_new() failed -- "
                         "privacy indicator disabled for this run\n");
        close(g_notify_fd);
        g_notify_fd = -1;
        return false;
    }

    g_context = pw_context_new(pw_thread_loop_get_loop(g_loop), NULL, 0);
    if (!g_context) {
        fprintf(stderr, "stickBar: pw_context_new() failed -- "
                         "privacy indicator disabled for this run\n");
        pw_thread_loop_destroy(g_loop);
        g_loop = NULL;
        close(g_notify_fd);
        g_notify_fd = -1;
        return false;
    }

    if (pw_thread_loop_start(g_loop) < 0) {
        fprintf(stderr, "stickBar: failed to start the PipeWire client thread -- "
                         "privacy indicator disabled for this run\n");
        pipewire_client_shutdown();
        return false;
    }

    // Locking here (rather than only once the loop is dispatching in earnest)
    // is required, not just tidy -- the thread-loop's own docs call for
    // holding this lock any time the app thread touches PipeWire objects
    // after pw_thread_loop_start(), since the loop thread may be dispatching
    // concurrently.
    pw_thread_loop_lock(g_loop);
    g_core = pw_context_connect(g_context, NULL, 0);
    if (!g_core) {
        pw_thread_loop_unlock(g_loop);
        fprintf(stderr, "stickBar: failed to connect to PipeWire -- "
                         "privacy indicator disabled for this run\n");
        pipewire_client_shutdown();
        return false;
    }
    g_registry = pw_core_get_registry(g_core, PW_VERSION_REGISTRY, 0);
    if (!g_registry) {
        pw_thread_loop_unlock(g_loop);
        fprintf(stderr, "stickBar: pw_core_get_registry() failed -- "
                         "privacy indicator disabled for this run\n");
        pipewire_client_shutdown();
        return false;
    }
    pw_registry_add_listener(g_registry, &g_registry_listener, &g_registry_events, NULL);
    pw_thread_loop_unlock(g_loop);

    return true;
}

void pipewire_client_shutdown(void) {
    if (g_loop) {
        pw_thread_loop_lock(g_loop);
        for (int i = 0; i < MAX_PW_NODES; i++) {
            if (g_nodes[i].in_use && g_nodes[i].proxy) {
                pw_proxy_destroy(g_nodes[i].proxy);
            }
        }
        memset(g_nodes, 0, sizeof(g_nodes));
        memset(g_links, 0, sizeof(g_links));
        if (g_core) {
            pw_core_disconnect(g_core);
            g_core = NULL;
        }
        g_registry = NULL;
        pw_thread_loop_unlock(g_loop);
    }

    if (g_context) {
        pw_context_destroy(g_context);
        g_context = NULL;
    }

    if (g_loop) {
        pw_thread_loop_stop(g_loop);
        pw_thread_loop_destroy(g_loop);
        g_loop = NULL;
    }

    if (g_notify_fd >= 0) {
        close(g_notify_fd);
        g_notify_fd = -1;
    }

    pw_deinit();
}

int pipewire_client_notify_fd(void) {
    return g_notify_fd;
}

void pipewire_client_pump(void) {
    if (g_notify_fd < 0) {
        return;
    }
    eventfd_t val;
    // Same "one read drains the whole accumulated counter" reasoning as
    // audio/pulse_client.c's pulse_client_pump().
    (void)eventfd_read(g_notify_fd, &val);
}

bool pipewire_client_mic_active(void) {
    if (!g_loop) return false;
    pw_thread_loop_lock(g_loop);
    bool active = false;
    for (int i = 0; i < MAX_PW_NODES && !active; i++) {
        PwNode *n = &g_nodes[i];
        if (node_is_active_stream(n) && strcmp(n->media_class, "Stream/Input/Audio") == 0) {
            active = true;
        }
    }
    pw_thread_loop_unlock(g_loop);
    return active;
}

bool pipewire_client_webcam_active(void) {
    if (!g_loop) return false;
    pw_thread_loop_lock(g_loop);
    bool active = false;
    for (int i = 0; i < MAX_PW_NODES && !active; i++) {
        PwNode *n = &g_nodes[i];
        if (node_is_active_stream(n) && strcmp(n->media_class, "Stream/Input/Video") == 0 &&
            video_node_is_webcam(n)) {
            active = true;
        }
    }
    pw_thread_loop_unlock(g_loop);
    return active;
}

bool pipewire_client_screenshare_active(void) {
    if (!g_loop) return false;
    pw_thread_loop_lock(g_loop);
    bool active = false;
    for (int i = 0; i < MAX_PW_NODES && !active; i++) {
        PwNode *n = &g_nodes[i];
        if (node_is_active_stream(n) && strcmp(n->media_class, "Stream/Input/Video") == 0 &&
            !video_node_is_webcam(n)) {
            active = true;
        }
    }
    pw_thread_loop_unlock(g_loop);
    return active;
}

bool pipewire_client_any_active(void) {
    return pipewire_client_mic_active() || pipewire_client_webcam_active() ||
           pipewire_client_screenshare_active();
}

int pipewire_client_entry_count(void) {
    if (!g_loop) return 0;
    pw_thread_loop_lock(g_loop);
    int count = 0;
    for (int i = 0; i < MAX_PW_NODES; i++) {
        PwNode *n = &g_nodes[i];
        if (node_is_active_stream(n) &&
            (strcmp(n->media_class, "Stream/Input/Audio") == 0 ||
             strcmp(n->media_class, "Stream/Input/Video") == 0)) {
            count++;
        }
    }
    pw_thread_loop_unlock(g_loop);
    return count;
}

bool pipewire_client_entry(int index, PrivacyCategory *category_out, char *app_name_out,
                            size_t app_name_out_len) {
    if (!g_loop) return false;
    pw_thread_loop_lock(g_loop);
    bool found = false;
    int seen = 0;
    for (int i = 0; i < MAX_PW_NODES; i++) {
        PwNode *n = &g_nodes[i];
        if (!node_is_active_stream(n)) {
            continue;
        }
        PrivacyCategory category;
        if (strcmp(n->media_class, "Stream/Input/Audio") == 0) {
            category = PRIVACY_CATEGORY_MIC;
        } else if (strcmp(n->media_class, "Stream/Input/Video") == 0) {
            category = video_node_is_webcam(n) ? PRIVACY_CATEGORY_WEBCAM : PRIVACY_CATEGORY_SCREENSHARE;
        } else {
            continue;
        }
        if (seen == index) {
            if (category_out) *category_out = category;
            if (app_name_out) snprintf(app_name_out, app_name_out_len, "%s", n->display_name);
            found = true;
            break;
        }
        seen++;
    }
    pw_thread_loop_unlock(g_loop);
    return found;
}
