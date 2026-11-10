#include "mpris/mpris_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "dbus/dbus_dict.h"
#include "wayland/output.h"

// Every MPRIS player lives at this same fixed object path, per spec --
// unlike SNI (dbus/sni_registry.c), there's no per-item path to discover.
#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT_INTERFACE "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define MPRIS_NAME_PREFIX "org.mpris.MediaPlayer2."

// Fixed-size, generously above what any real desktop would run at once --
// matches this project's existing convention (sni_registry.c's
// MAX_TRAY_ITEMS, workspaces.c's MAX_TRACKED_OUTPUTS) of a bounded array over
// dynamic growth. Slots are never moved/compacted once handed out, same
// reasoning as sni_registry.c's struct SniItem: a slot's address is the fixed
// userdata pointer handed to sd_bus_match_signal().
#define MAX_MPRIS_PLAYERS 16

struct MprisPlayer {
    bool in_use;
    sd_bus *bus;
    char bus_name[256];
    char identity[128];
    char status[16]; // "Playing" | "Paused" | "Stopped" | "" (never fetched yet)
    char title[256];
    char artist[256];
    char album[256]; // "" if unreported (common: browsers send an empty one)
    bool can_go_next, can_go_previous;
    bool can_seek;       // CanSeek -- gates mpris_player_seek_to()
    char track_id[256];  // Metadata's mpris:trackid, "" if unreported -- SetPosition() needs it
    int64_t length_us;   // 0 if unreported
    int64_t position_us; // last value from mpris_player_refresh_position(), 0 if never polled
    pid_t pid;           // process owning bus_name, 0 if unknown -- see is_shadowed()
    sd_bus_slot *signal_slot;
};

static struct MprisPlayer g_players[MAX_MPRIS_PLAYERS];
static AppContext *g_app = NULL;

static MprisPlayer *find_player(const char *bus_name) {
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        if (g_players[i].in_use && strcmp(g_players[i].bus_name, bus_name) == 0) {
            return &g_players[i];
        }
    }
    return NULL;
}

static MprisPlayer *find_free_slot(void) {
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        if (!g_players[i].in_use) {
            return &g_players[i];
        }
    }
    return NULL;
}

// True for browser-engine per-process MPRIS names ("...chromium.instance57585",
// "...firefox.instance_1_23") as opposed to an app's own name ("...spotify").
static bool is_instance_name(const char *bus_name) {
    return strstr(bus_name, ".instance") != NULL;
}

/*
 * One process can own more than one MPRIS name, and when it does they're the
 * same playback reported twice. Seen live with Spotify (an Electron/Chromium
 * app): PID 57585 owned both org.mpris.MediaPlayer2.spotify and
 * org.mpris.MediaPlayer2.chromium.instance57585 -- identical length and
 * position, but the Chromium mirror's metadata was *stale* (an earlier track's
 * title while Spotify's own name reported the real one). Tracking both showed
 * Spotify twice in the dropdown, and the bar widget showed the wrong song
 * whenever the stale twin happened to come first.
 *
 * So per process only one player is visible: an app's own name beats a
 * ".instance" browser-engine mirror, otherwise the lower slot wins. A real
 * standalone browser still shows up -- nothing else shares its PID. Players
 * whose PID couldn't be read (0) are never hidden. Computed on demand rather
 * than cached, so it fixes itself as names come and go (if Spotify's own name
 * disappears, its mirror becomes visible again).
 */
static bool is_shadowed(const MprisPlayer *p) {
    if (p->pid <= 0) {
        return false;
    }
    bool p_instance = is_instance_name(p->bus_name);
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        const MprisPlayer *o = &g_players[i];
        if (o == p || !o->in_use || o->pid != p->pid) {
            continue;
        }
        bool o_instance = is_instance_name(o->bus_name);
        if (p_instance && !o_instance) {
            return true; // the app's own name wins over its engine mirror
        }
        if (p_instance == o_instance && o < p) {
            return true; // same kind -- lower slot wins
        }
    }
    return false;
}

static bool is_visible(const MprisPlayer *p) {
    return p->in_use && !is_shadowed(p);
}

int mpris_player_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        if (is_visible(&g_players[i])) {
            count++;
        }
    }
    return count;
}

MprisPlayer *mpris_player_get(int index) {
    int seen = 0;
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        if (is_visible(&g_players[i])) {
            if (seen == index) {
                return &g_players[i];
            }
            seen++;
        }
    }
    return NULL;
}

MprisPlayer *mpris_active_player(void) {
    MprisPlayer *first = NULL;
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        if (!is_visible(&g_players[i])) {
            continue;
        }
        if (!first) {
            first = &g_players[i];
        }
        if (strcmp(g_players[i].status, "Playing") == 0) {
            return &g_players[i];
        }
    }
    return first;
}

const char *mpris_player_title(MprisPlayer *player) { return player->title; }
int mpris_player_pid(MprisPlayer *player) { return (int)player->pid; }
const char *mpris_player_artist(MprisPlayer *player) { return player->artist; }
const char *mpris_player_identity(MprisPlayer *player) { return player->identity; }

void mpris_player_app_name(MprisPlayer *player, char *out, size_t out_size) {
    if (player->identity[0]) {
        snprintf(out, out_size, "%s", player->identity);
        return;
    }
    // No Identity reported -- fall back to the bus name's own suffix
    // ("org.mpris.MediaPlayer2.vlc" -> "vlc"), minus any ".instance..." tail.
    const char *name = player->bus_name;
    size_t prefix_len = strlen(MPRIS_NAME_PREFIX);
    if (strncmp(name, MPRIS_NAME_PREFIX, prefix_len) == 0) {
        name += prefix_len;
    }
    const char *instance = strstr(name, ".instance");
    int len = instance ? (int)(instance - name) : (int)strlen(name);
    snprintf(out, out_size, "%.*s", len, name);
}
bool mpris_player_is_playing(MprisPlayer *player) { return strcmp(player->status, "Playing") == 0; }
const char *mpris_player_status(MprisPlayer *player) { return player->status; }
const char *mpris_player_album(MprisPlayer *player) { return player->album; }
bool mpris_player_can_go_next(MprisPlayer *player) { return player->can_go_next; }
bool mpris_player_can_go_previous(MprisPlayer *player) { return player->can_go_previous; }
int64_t mpris_player_length_us(MprisPlayer *player) { return player->length_us; }
int64_t mpris_player_position_us(MprisPlayer *player) { return player->position_us; }

void mpris_player_refresh_position(MprisPlayer *player) {
    if (!player || !player->bus) {
        return;
    }
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    if (sd_bus_get_property(player->bus, player->bus_name, MPRIS_OBJECT_PATH, MPRIS_PLAYER_INTERFACE, "Position",
                             &err, &reply, "x") >= 0) {
        int64_t pos = 0;
        if (sd_bus_message_read_basic(reply, SD_BUS_TYPE_INT64, &pos) > 0) {
            player->position_us = pos;
        }
    }
    sd_bus_error_free(&err);
    if (reply) {
        sd_bus_message_unref(reply);
    }
}

static void send_player_call(MprisPlayer *player, const char *method) {
    if (!player || !player->bus) {
        return;
    }
    sd_bus_message *msg = NULL;
    if (sd_bus_message_new_method_call(player->bus, &msg, player->bus_name, MPRIS_OBJECT_PATH,
                                        MPRIS_PLAYER_INTERFACE, method) < 0) {
        return;
    }
    sd_bus_send(player->bus, msg, NULL);
    sd_bus_message_unref(msg);
}

void mpris_player_toggle_playing(MprisPlayer *player) {
    send_player_call(player, "PlayPause");
}

void mpris_player_next(MprisPlayer *player) {
    if (!player || !player->can_go_next) {
        return;
    }
    send_player_call(player, "Next");
}

void mpris_player_previous(MprisPlayer *player) {
    if (!player || !player->can_go_previous) {
        return;
    }
    send_player_call(player, "Previous");
}

bool mpris_player_can_seek(MprisPlayer *player) { return player && player->can_seek; }

void mpris_player_seek_to(MprisPlayer *player, int64_t position_us) {
    if (!player || !player->bus || !player->can_seek) {
        return;
    }
    if (position_us < 0) {
        position_us = 0;
    }
    // SetPosition is the spec's absolute seek, but it names the track it's
    // meant for (so a seek racing a track change is ignored, not applied to
    // the next song) -- without a track id, fall back to the relative Seek,
    // measured from the last polled position.
    bool absolute = player->track_id[0] != '\0';
    sd_bus_message *msg = NULL;
    if (sd_bus_message_new_method_call(player->bus, &msg, player->bus_name, MPRIS_OBJECT_PATH,
                                        MPRIS_PLAYER_INTERFACE, absolute ? "SetPosition" : "Seek") < 0) {
        return;
    }
    int r = absolute ? sd_bus_message_append(msg, "ox", player->track_id, position_us)
                     : sd_bus_message_append(msg, "x", position_us - player->position_us);
    if (r >= 0) {
        // Fire-and-forget, like the other controls (see the header).
        sd_bus_send(player->bus, msg, NULL);
        // Optimistic: the dropdown shows the new spot right away instead of
        // waiting up to a second for its next Position poll, which then
        // corrects it if the player clamped or ignored the seek.
        player->position_us = position_us;
    }
    sd_bus_message_unref(msg);
}

static bool metadata_entry(sd_bus_message *m, const char *key, const char *contents, void *user_data) {
    MprisPlayer *item = user_data;
    if (strcmp(key, "xesam:title") == 0) {
        return dbus_dict_read_string(m, contents, item->title, sizeof(item->title));
    }
    if (strcmp(key, "xesam:album") == 0) {
        return dbus_dict_read_string(m, contents, item->album, sizeof(item->album));
    }
    if (strcmp(key, "xesam:artist") == 0 && strcmp(contents, "as") == 0) {
        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s") > 0) {
            // Keep only the first artist, but still drain every remaining
            // entry so the cursor ends up past the whole array -- required
            // before exit_container(), same reasoning as
            // sni_registry.c's convert_best_pixmap_from_message() draining
            // every pixmap entry rather than stopping early.
            const char *artist = NULL;
            while (sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &artist) > 0) {
                if (artist && !item->artist[0]) {
                    snprintf(item->artist, sizeof(item->artist), "%s", artist);
                }
            }
            sd_bus_message_exit_container(m);
        }
        return true;
    }
    if (strcmp(key, "mpris:trackid") == 0 && (strcmp(contents, "o") == 0 || strcmp(contents, "s") == 0)) {
        // The spec says an object path ("o"); some players send a plain
        // string. Either way it has to be a valid object path to go back out
        // in SetPosition(), so a string that isn't one is dropped (seeking
        // then falls back to the relative Seek()).
        const char *id = NULL;
        sd_bus_message_read_basic(m, contents[0], &id);
        if (id && sd_bus_object_path_is_valid(id) && strlen(id) < sizeof(item->track_id)) {
            snprintf(item->track_id, sizeof(item->track_id), "%s", id);
        }
        return true;
    }
    if (strcmp(key, "mpris:length") == 0 && strcmp(contents, "x") == 0) {
        int64_t len = 0;
        sd_bus_message_read_basic(m, SD_BUS_TYPE_INT64, &len);
        item->length_us = len;
        return true;
    }
    if (strcmp(key, "mpris:length") == 0 && strcmp(contents, "t") == 0) {
        // The spec says int64 ("x"), but Spotify sends uint64 ("t") --
        // checked live with busctl. Accepting only "x" left Spotify's
        // length at 0, so its dropdown timeline never showed.
        uint64_t len = 0;
        sd_bus_message_read_basic(m, SD_BUS_TYPE_UINT64, &len);
        item->length_us = len > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)len;
        return true;
    }
    return false;
}

// Parses an "a{sv}" MPRIS Metadata dict positioned at the message's current
// read cursor (already inside the outer variant that wraps it -- same shape
// as sni_registry.c's convert_best_pixmap_from_message() being called from
// inside fetch_item_properties()'s own variant-entry loop). xesam:artist is
// an array of strings per spec (a track can have multiple credited artists)
// -- only the first is kept, matching the reference trackText()'s own
// single-artist display.
static void parse_metadata(sd_bus_message *m, MprisPlayer *item) {
    item->title[0] = '\0';
    item->artist[0] = '\0';
    item->album[0] = '\0';
    item->track_id[0] = '\0';
    item->length_us = 0;

    dbus_dict_walk(m, metadata_entry, item);
}

// One Player property from GetAll (or, later, PropertiesChanged).
static bool player_entry(sd_bus_message *m, const char *key, const char *contents, void *user_data) {
    MprisPlayer *item = user_data;
    if (strcmp(key, "PlaybackStatus") == 0) {
        return dbus_dict_read_string(m, contents, item->status, sizeof(item->status));
    }
    if (strcmp(key, "Metadata") == 0 && strcmp(contents, "a{sv}") == 0) {
        parse_metadata(m, item);
        return true;
    }
    if (strcmp(contents, "b") == 0 &&
        (strcmp(key, "CanGoNext") == 0 || strcmp(key, "CanGoPrevious") == 0 || strcmp(key, "CanSeek") == 0)) {
        int v = 0;
        sd_bus_message_read_basic(m, SD_BUS_TYPE_BOOLEAN, &v);
        if (strcmp(key, "CanGoNext") == 0) {
            item->can_go_next = v != 0;
        } else if (strcmp(key, "CanGoPrevious") == 0) {
            item->can_go_previous = v != 0;
        } else {
            item->can_seek = v != 0; // not in PlayerView: nothing drawn depends on it
        }
        return true;
    }
    return false;
}

// Re-fetches every Player-interface property this module cares about (a full
// GetAll, not an incremental parse of PropertiesChanged's own changed-
// properties payload) plus the root interface's Identity -- simpler and more
// robust than tracking partial updates, at the cost of one extra round trip
// per change. Acceptable here since PropertiesChanged fires relatively
// rarely (track changes, play/pause, not a hot path), same tradeoff
// dbus/sni_registry.c's refresh_icon() already makes for NewIcon.
// GetAll on the Player interface, plus (with_identity) the root interface's
// Identity -- once when the player appears; Identity doesn't change.
static void fetch_player_properties(MprisPlayer *item, bool with_identity) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_call_method(item->bus, item->bus_name, MPRIS_OBJECT_PATH,
                                 "org.freedesktop.DBus.Properties", "GetAll",
                                 &err, &reply, "s", MPRIS_PLAYER_INTERFACE);
    if (rc < 0) {
        fprintf(stderr, "stickBar: failed to fetch MPRIS properties for %s (%s)\n", item->bus_name,
                err.message ? err.message : "unknown error");
        sd_bus_error_free(&err);
        return;
    }

    dbus_dict_walk(reply, player_entry, item);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    if (!with_identity) {
        return;
    }

    // Identity lives on the root org.mpris.MediaPlayer2 interface, not
    // Player -- fetched as a second, separate call. Best-effort: a player
    // that fails to answer this (spec says every player must implement it,
    // but real-world robustness costs nothing here) just keeps whatever
    // identity string (possibly empty) it already had.
    sd_bus_error id_err = SD_BUS_ERROR_NULL;
    sd_bus_message *id_reply = NULL;
    if (sd_bus_get_property(item->bus, item->bus_name, MPRIS_OBJECT_PATH, MPRIS_ROOT_INTERFACE, "Identity",
                             &id_err, &id_reply, "s") >= 0) {
        const char *identity = NULL;
        if (sd_bus_message_read_basic(id_reply, SD_BUS_TYPE_STRING, &identity) > 0 && identity) {
            snprintf(item->identity, sizeof(item->identity), "%s", identity);
        }
    }
    sd_bus_error_free(&id_err);
    if (id_reply) {
        sd_bus_message_unref(id_reply);
    }
}

// Bumped whenever anything a player shows changes, or players come and go --
// see mpris_change_count().
static unsigned g_change_count = 0;

// Something visible changed: redraw the bar, and let the dropdown know.
static void players_changed(void) {
    g_change_count++;
    if (g_app) {
        output_mark_all_dirty(g_app);
    }
}

// What a player shows, for telling whether a PropertiesChanged changed any of
// it (players also send it for volume, rate, shuffle, ... which nothing here
// displays).
typedef struct {
    char status[16], title[256], artist[256], album[256];
    bool can_go_next, can_go_previous;
    int64_t length_us;
} PlayerView;

static void player_view(const MprisPlayer *p, PlayerView *v) {
    memcpy(v->status, p->status, sizeof(v->status));
    memcpy(v->title, p->title, sizeof(v->title));
    memcpy(v->artist, p->artist, sizeof(v->artist));
    memcpy(v->album, p->album, sizeof(v->album));
    v->can_go_next = p->can_go_next;
    v->can_go_previous = p->can_go_previous;
    v->length_us = p->length_us;
}

static bool player_view_equal(const PlayerView *a, const PlayerView *b) {
    return strcmp(a->status, b->status) == 0 && strcmp(a->title, b->title) == 0 &&
           strcmp(a->artist, b->artist) == 0 && strcmp(a->album, b->album) == 0 &&
           a->can_go_next == b->can_go_next && a->can_go_previous == b->can_go_previous &&
           a->length_us == b->length_us;
}

// PropertiesChanged(s interface, a{sv} changed, as invalidated): the new
// values are right there in the signal, so it's parsed directly -- this used
// to make two *blocking* round trips to the player (GetAll + Get(Identity))
// per signal, which browsers send for every volume/rate/metadata tweak, and a
// hung player could stall the whole bar for sd-bus's 25s timeout. Only
// properties sent as "invalidated" (name only, no value) need a GetAll.
static void apply_properties_changed(sd_bus_message *m, MprisPlayer *item) {
    const char *iface = NULL;
    if (sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &iface) < 0 || !iface ||
        strcmp(iface, MPRIS_PLAYER_INTERFACE) != 0) {
        return; // the root interface (Identity etc.), or another one: nothing shown changes
    }
    PlayerView before, after;
    player_view(item, &before);
    dbus_dict_walk(m, player_entry, item);
    bool invalidated = false;
    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s") > 0) {
        const char *name = NULL;
        while (sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &name) > 0) {
            invalidated = true;
        }
        sd_bus_message_exit_container(m);
    }
    if (invalidated) {
        fetch_player_properties(item, false);
    }
    player_view(item, &after);
    if (!player_view_equal(&before, &after)) {
        players_changed();
    }
}

static int on_player_signal(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    MprisPlayer *item = userdata;
    if (!item->in_use) {
        // Defensive: shouldn't happen, since the slot is unref'd in
        // remove_player() before the struct is reset -- same guard
        // dbus/sni_registry.c's on_item_signal() keeps for the same reason.
        return 0;
    }
    const char *member = sd_bus_message_get_member(m);
    if (member && strcmp(member, "PropertiesChanged") == 0) {
        apply_properties_changed(m, item);
    }
    return 0;
}

// Reference MediaPlayer.qml filters out playerctld (a shadow/mirror service
// that duplicates whatever's actually playing under its own MPRIS name) so
// it doesn't show up as a second, redundant entry -- same exclusion here.
static bool is_playerctld(const char *bus_name) {
    return strstr(bus_name, "playerctld") != NULL;
}

static void add_player(sd_bus *bus, const char *bus_name) {
    if (is_playerctld(bus_name) || find_player(bus_name)) {
        return;
    }
    MprisPlayer *item = find_free_slot();
    if (!item) {
        fprintf(stderr, "stickBar: MPRIS player limit (%d) reached, ignoring %s\n", MAX_MPRIS_PLAYERS, bus_name);
        return;
    }

    memset(item, 0, sizeof(*item));
    item->in_use = true;
    item->bus = bus;
    snprintf(item->bus_name, sizeof(item->bus_name), "%s", bus_name);

    // Owning process, for is_shadowed()'s same-process dedup. Best-effort: if
    // the bus won't say, pid stays 0 and the player is simply never hidden.
    sd_bus_creds *creds = NULL;
    if (sd_bus_get_name_creds(bus, bus_name, SD_BUS_CREDS_PID, &creds) >= 0) {
        pid_t pid = 0;
        if (sd_bus_creds_get_pid(creds, &pid) >= 0) {
            item->pid = pid;
        }
        sd_bus_creds_unref(creds);
    }

    fetch_player_properties(item, true);

    // member=NULL subscribes to every signal from the Properties interface at
    // this player's object path (in practice, only PropertiesChanged) --
    // same "match broadly, dispatch by name inside the callback" convention
    // as dbus/sni_registry.c's sni_registry_add(). bus_name here is the
    // well-known "org.mpris.MediaPlayer2.*" name, not a unique connection
    // name -- confirmed to work as a match-rule sender the same way
    // sni_registry.c already relies on for its own per-item subscriptions.
    sd_bus_match_signal(bus, &item->signal_slot, item->bus_name, MPRIS_OBJECT_PATH,
                         "org.freedesktop.DBus.Properties", NULL, on_player_signal, item);

    players_changed();
}

static void remove_player(const char *bus_name) {
    MprisPlayer *item = find_player(bus_name);
    if (!item) {
        return;
    }
    if (item->signal_slot) {
        sd_bus_slot_unref(item->signal_slot);
    }
    memset(item, 0, sizeof(*item)); // in_use = false
    players_changed();
}

// Fires for every bus-name ownership change on the whole session bus, not
// just MPRIS ones -- same "global match, filter inside the callback"
// approach dbus/sni_watcher_service.c's own on_name_owner_changed() already
// takes for the exact same reason (D-Bus match rules can't filter an
// arbitrary name prefix without a raw match-string API this codebase doesn't
// otherwise use). An empty new_owner means that name's previous owner
// disconnected (the player quit); a non-empty one means a player just
// appeared and claimed its well-known name.
static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    sd_bus *bus = userdata;
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    (void)old_owner;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || !name) {
        return 0;
    }
    if (strncmp(name, MPRIS_NAME_PREFIX, strlen(MPRIS_NAME_PREFIX)) != 0) {
        return 0;
    }
    if (new_owner && new_owner[0] != '\0') {
        add_player(bus, name);
    } else {
        remove_player(name);
    }
    return 0;
}

static void fetch_initial_players(sd_bus *bus) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc =
        sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames",
                            &err, &reply, "");
    if (rc < 0) {
        fprintf(stderr, "stickBar: failed to list D-Bus names for MPRIS discovery (%s)\n",
                err.message ? err.message : "unknown error");
        sd_bus_error_free(&err);
        return;
    }

    char **names = NULL;
    if (sd_bus_message_read_strv(reply, &names) >= 0 && names) {
        for (char **p = names; *p; p++) {
            if (strncmp(*p, MPRIS_NAME_PREFIX, strlen(MPRIS_NAME_PREFIX)) == 0) {
                add_player(bus, *p);
            }
            free(*p);
        }
        free(names);
    }
    sd_bus_message_unref(reply);
}

void mpris_client_init(sd_bus *bus, AppContext *app) {
    g_app = app;
    fetch_initial_players(bus);

    sd_bus_slot *slot = NULL;
    sd_bus_match_signal(bus, &slot, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         "NameOwnerChanged", on_name_owner_changed, bus);
    // `slot` is intentionally left unref'd for the rest of the process's
    // life, same convention as every other long-lived subscription in this
    // codebase (dbus/sni_watcher.c's registered_slot, etc.).
}

unsigned mpris_change_count(void) {
    return g_change_count;
}
