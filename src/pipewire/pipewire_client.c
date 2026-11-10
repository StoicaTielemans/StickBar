#include "pipewire/pipewire_client.h"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/file.h"
#include "util/notify_fd.h"
#include "visualizer/visualizer.h" // VISUALIZER_NODE_NAME

// Fixed-size, matching this project's existing convention (audio/pulse_client.c's
// MAX_PULSE_NODES, dbus/sni_registry.c's MAX_TRAY_ITEMS) of a bounded array over
// dynamic growth. A real desktop's PipeWire graph (audio devices, sinks/sources,
// playback/capture streams, filters) commonly runs in the 50-150 node range;
// 256 is generous headroom for both tables without being unbounded.
#define MAX_PW_NODES 256
#define MAX_PW_LINKS 256
// Client objects (one per connection: apps, pipewire-pulse's per-app clients,
// the session manager, ...) -- far fewer than nodes.
#define MAX_PW_CLIENTS 128

typedef struct {
    bool in_use;
    uint32_t id; // registry global id
    char media_class[64];
    char display_name[128];

    // From the registry global's props: object.serial (what a capture
    // stream's target.object names) and the owning client's global id. Used
    // for Stream/Output/Audio nodes, to find a media player's own playback
    // stream (pipewire_find_playback_for_pid()).
    uint32_t serial;
    uint32_t client_id;

    // Only meaningful when is_tracked_stream is true (Stream/Input/Audio,
    // Stream/Input/Video, or Stream/Output/Audio) -- every other node (sinks,
    // sources, filters, ...) is still tracked (its media_class is needed as a
    // possible *link source* for classify-webcam-vs-screenshare below) but
    // never bound, so `running`/`proxy` stay at their zeroed defaults for it.
    // The privacy accessors below match Stream/Input/* by name, so tracking
    // playback streams here never shows up in the privacy indicator.
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

// A client's process id, for matching playback streams to MPRIS players.
// The registry global only carries pipewire.sec.pid -- the *connecting*
// process, which for everything going through pipewire-pulse (browsers,
// Steam, ...) is pipewire-pulse itself (seen live: all such clients showed
// the same daemon pid). The real app pid is application.process.id, which
// only the bound client's info carries, so every client is bound for that.
typedef struct {
    bool in_use;
    uint32_t id;
    int pid; // application.process.id if known, else pipewire.sec.pid, else 0
    struct pw_proxy *proxy;
    struct spa_hook proxy_listener;
} PwClient;

static struct pw_thread_loop *g_loop = NULL;
static struct pw_context *g_context = NULL;
static struct pw_core *g_core = NULL;
static struct pw_registry *g_registry = NULL;
static struct spa_hook g_registry_listener;
static int g_notify_fd = -1;

static PwNode g_nodes[MAX_PW_NODES];
static PwClient g_clients[MAX_PW_CLIENTS];

// Bumped (under the lock) whenever something a player <-> playback stream
// match depends on changes: a playback stream appearing, going, or starting /
// stopping, or a client's pid becoming known. pipewire_client_pump() reports
// it, so the media dropdown only re-matches when it could make a difference.
static unsigned g_playback_gen = 0;

// Main thread only: the privacy state as of the last pump. Cached so the bar
// is redrawn only when it actually changed, and so privacy.c's per-frame reads
// don't each take the lock and scan every node.
static bool g_mic_active = false, g_webcam_active = false, g_screenshare_active = false;
static unsigned g_seen_playback_gen = 0;
static PwLink g_links[MAX_PW_LINKS];

// Wakes up the main epoll loop to re-read the cache and redraw. Safe to call
// from inside a PipeWire callback (thread-loop thread, lock held) -- see
// util/notify_fd.h.
static void notify_dirty(void) {
    notify_fd_signal(g_notify_fd);
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
        if (strcmp(node->media_class, "Stream/Output/Audio") == 0) {
            g_playback_gen++;
        }
        notify_dirty();
    }
}

static const struct pw_node_events g_node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = node_info_cb,
};

static void client_info_cb(void *data, const struct pw_client_info *info) {
    PwClient *client = data;
    if (!info || !info->props) {
        return;
    }
    const char *pid = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID);
    if (pid && pid[0]) {
        client->pid = atoi(pid);
        g_playback_gen++; // a stream may now match a player
        notify_dirty();
    }
}

static const struct pw_client_events g_client_events = {
    .version = PW_VERSION_CLIENT_EVENTS,
    .info = client_info_cb,
};

static void handle_client_global(uint32_t id, const struct spa_dict *props) {
    PwClient *client = NULL;
    for (int i = 0; i < MAX_PW_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            client = &g_clients[i];
            break;
        }
    }
    if (!client) {
        return; // only costs a possible player<->stream match
    }
    memset(client, 0, sizeof(*client));
    client->in_use = true;
    client->id = id;
    const char *sec_pid = props ? spa_dict_lookup(props, PW_KEY_SEC_PID) : NULL;
    client->pid = sec_pid ? atoi(sec_pid) : 0; // until the info arrives
    struct pw_proxy *proxy = pw_registry_bind(g_registry, id, PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT, 0);
    if (proxy) {
        client->proxy = proxy;
        pw_client_add_listener((struct pw_client *)proxy, &client->proxy_listener, &g_client_events, client);
    }
}

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
    const char *serial = props ? spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL) : NULL;
    const char *client_id = props ? spa_dict_lookup(props, PW_KEY_CLIENT_ID) : NULL;
    node->serial = serial ? (uint32_t)strtoul(serial, NULL, 10) : 0;
    node->client_id = client_id ? (uint32_t)strtoul(client_id, NULL, 10) : 0;

    // Only Stream/Input/{Audio,Video} nodes need their live running state
    // watched -- everything else (sinks, real device source nodes used only
    // as a link's *source* for the webcam/screen-share classification below,
    // filters, ...) just needs its media_class cached, never bound.
    //
    // The bar's own media visualizer (visualizer/visualizer.c) is a
    // Stream/Input/Audio node too -- while the media dropdown is open it
    // captures each playing player's audio. Left tracked, the privacy box
    // would show a mic icon then, naming stickBar itself; it's still
    // kept in the table (harmless, and it keeps link lookups uniform), just
    // never counted as an active stream.
    const char *node_name = props ? spa_dict_lookup(props, PW_KEY_NODE_NAME) : NULL;
    bool is_own_visualizer = node_name && strcmp(node_name, VISUALIZER_NODE_NAME) == 0;
    bool is_audio_input = !is_own_visualizer && strcmp(node->media_class, "Stream/Input/Audio") == 0;
    bool is_video_input = strcmp(node->media_class, "Stream/Input/Video") == 0;
    bool is_audio_output = strcmp(node->media_class, "Stream/Output/Audio") == 0;
    if (is_audio_output) {
        g_playback_gen++;
    }
    if (is_audio_input || is_video_input || is_audio_output) {
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
    } else if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        handle_client_global(id, props);
    }
}

static void registry_event_global_remove(void *data, uint32_t id) {
    (void)data;
    for (int i = 0; i < MAX_PW_NODES; i++) {
        if (g_nodes[i].in_use && g_nodes[i].id == id) {
            if (g_nodes[i].proxy) {
                pw_proxy_destroy(g_nodes[i].proxy);
            }
            if (strcmp(g_nodes[i].media_class, "Stream/Output/Audio") == 0) {
                g_playback_gen++;
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
    for (int i = 0; i < MAX_PW_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].id == id) {
            if (g_clients[i].proxy) {
                pw_proxy_destroy(g_clients[i].proxy);
            }
            memset(&g_clients[i], 0, sizeof(g_clients[i]));
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
// interactive UI a headless test can't provide) -- see
// docs/modules/privacy.md for what to re-check first if this is ever wrong on
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

    g_notify_fd = notify_fd_new("pipewire notify");
    if (g_notify_fd < 0) {
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
        for (int i = 0; i < MAX_PW_CLIENTS; i++) {
            if (g_clients[i].in_use && g_clients[i].proxy) {
                pw_proxy_destroy(g_clients[i].proxy);
            }
        }
        memset(g_nodes, 0, sizeof(g_nodes));
        memset(g_links, 0, sizeof(g_links));
        memset(g_clients, 0, sizeof(g_clients));
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

unsigned pipewire_client_pump(void) {
    notify_fd_drain(g_notify_fd);
    if (!g_loop) {
        return 0;
    }
    // One scan for all three categories, under one lock.
    bool mic = false, webcam = false, screenshare = false;
    pw_thread_loop_lock(g_loop);
    for (int i = 0; i < MAX_PW_NODES; i++) {
        PwNode *n = &g_nodes[i];
        if (!node_is_active_stream(n)) {
            continue;
        }
        if (strcmp(n->media_class, "Stream/Input/Audio") == 0) {
            mic = true;
        } else if (strcmp(n->media_class, "Stream/Input/Video") == 0) {
            if (video_node_is_webcam(n)) {
                webcam = true;
            } else {
                screenshare = true;
            }
        }
    }
    unsigned playback_gen = g_playback_gen;
    pw_thread_loop_unlock(g_loop);

    unsigned changed = 0;
    if (mic != g_mic_active || webcam != g_webcam_active || screenshare != g_screenshare_active) {
        changed |= PIPEWIRE_PRIVACY_CHANGED;
    }
    if (playback_gen != g_seen_playback_gen) {
        changed |= PIPEWIRE_PLAYBACK_CHANGED;
    }
    g_mic_active = mic;
    g_webcam_active = webcam;
    g_screenshare_active = screenshare;
    g_seen_playback_gen = playback_gen;
    return changed;
}

bool pipewire_client_mic_active(void) { return g_mic_active; }
bool pipewire_client_webcam_active(void) { return g_webcam_active; }
bool pipewire_client_screenshare_active(void) { return g_screenshare_active; }

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

// --- Player <-> playback stream matching ---

// Whether `pid` is `ancestor` or one of its descendants, by walking PPid: in
// /proc/<pid>/status. Browsers play audio from a child process (Chromium's
// audio service, a Firefox content process), while their MPRIS name is owned
// by the main process -- so an exact pid match alone would miss them. Bounded
// depth: real process trees are shallow, and it guards against a loop.
static bool is_same_or_descendant(int pid, int ancestor) {
    for (int depth = 0; depth < 16 && pid > 1; depth++) {
        if (pid == ancestor) {
            return true;
        }
        if (!proc_read_ppid(pid, &pid)) {
            return false;
        }
    }
    return false;
}

bool pipewire_find_playback_for_pid(int pid, uint32_t *serial_out) {
    if (!g_loop || pid <= 0) {
        return false;
    }
    // Snapshot the candidates under the lock; the /proc walk happens outside
    // it so the PipeWire thread isn't held up by file reads.
    struct { uint32_t serial; int pid; bool running; } cand[32];
    int n = 0;
    pw_thread_loop_lock(g_loop);
    for (int i = 0; i < MAX_PW_NODES && n < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
        PwNode *node = &g_nodes[i];
        if (!node->in_use || node->serial == 0 || strcmp(node->media_class, "Stream/Output/Audio") != 0) {
            continue;
        }
        int client_pid = 0;
        for (int c = 0; c < MAX_PW_CLIENTS; c++) {
            if (g_clients[c].in_use && g_clients[c].id == node->client_id) {
                client_pid = g_clients[c].pid;
                break;
            }
        }
        if (client_pid > 0) {
            cand[n].serial = node->serial;
            cand[n].pid = client_pid;
            cand[n].running = node->running;
            n++;
        }
    }
    pw_thread_loop_unlock(g_loop);

    // An app can hold several playback streams (Spotify keeps an idle one;
    // a browser has one per playing tab) -- prefer one that's running.
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (!is_same_or_descendant(cand[i].pid, pid)) {
            continue;
        }
        if (!found || cand[i].running) {
            *serial_out = cand[i].serial;
            found = true;
            if (cand[i].running) {
                break;
            }
        }
    }
    return found;
}
