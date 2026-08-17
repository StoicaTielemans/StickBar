#include "mpris/mpris_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpris/mpris_art.h"
#include "wayland/output.h"

// Album art is decoded/scaled once per track change (mpris_art_load(), see
// parse_metadata() below) to this fixed square size, then drawn scaled into
// whatever thumbnail slot the popup actually renders -- a generic size
// decided here rather than importing modules/media/media_popup.c's own
// THUMB_SIZE UI constant, keeping this data-layer file free of any
// popup-specific knowledge (same "no D-Bus types in this file" kind of
// domain boundary dbus/sni_icon.c already draws, just for a different pair
// of domains).
#define MPRIS_ART_SIZE 64

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
    bool can_go_next, can_go_previous;
    int64_t length_us;   // 0 if unreported
    int64_t position_us; // last value from mpris_player_refresh_position(), 0 if never polled
    char art_url[512];   // last-seen mpris:artUrl, "" if none -- used only to detect a real change
    cairo_surface_t *art_surface; // NULL if no usable art
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

int mpris_player_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        if (g_players[i].in_use) {
            count++;
        }
    }
    return count;
}

MprisPlayer *mpris_player_get(int index) {
    int seen = 0;
    for (int i = 0; i < MAX_MPRIS_PLAYERS; i++) {
        if (g_players[i].in_use) {
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
        if (!g_players[i].in_use) {
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
const char *mpris_player_artist(MprisPlayer *player) { return player->artist; }
const char *mpris_player_identity(MprisPlayer *player) { return player->identity; }
bool mpris_player_is_playing(MprisPlayer *player) { return strcmp(player->status, "Playing") == 0; }
bool mpris_player_can_go_next(MprisPlayer *player) { return player->can_go_next; }
bool mpris_player_can_go_previous(MprisPlayer *player) { return player->can_go_previous; }
int64_t mpris_player_length_us(MprisPlayer *player) { return player->length_us; }
int64_t mpris_player_position_us(MprisPlayer *player) { return player->position_us; }
cairo_surface_t *mpris_player_art_surface(MprisPlayer *player) { return player->art_surface; }

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
    item->length_us = 0;
    char new_art_url[sizeof(item->art_url)];
    new_art_url[0] = '\0';

    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
        return;
    }
    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
        const char *key = NULL;
        sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &key);

        char type = 0;
        const char *contents = NULL;
        sd_bus_message_peek_type(m, &type, &contents);

        if (contents && sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents) > 0) {
            if (key && strcmp(key, "xesam:title") == 0 && strcmp(contents, "s") == 0) {
                const char *title = NULL;
                sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &title);
                if (title) {
                    snprintf(item->title, sizeof(item->title), "%s", title);
                }
            } else if (key && strcmp(key, "xesam:artist") == 0 && strcmp(contents, "as") == 0) {
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
            } else if (key && strcmp(key, "mpris:length") == 0 && strcmp(contents, "x") == 0) {
                int64_t len = 0;
                sd_bus_message_read_basic(m, SD_BUS_TYPE_INT64, &len);
                item->length_us = len;
            } else if (key && strcmp(key, "mpris:artUrl") == 0 && strcmp(contents, "s") == 0) {
                const char *url = NULL;
                sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &url);
                if (url) {
                    snprintf(new_art_url, sizeof(new_art_url), "%s", url);
                }
            } else {
                sd_bus_message_skip(m, contents);
            }
            sd_bus_message_exit_container(m); // variant
        }
        sd_bus_message_exit_container(m); // dict entry
    }
    sd_bus_message_exit_container(m); // array

    // Only re-decode art when the URL string actually changed (a real track
    // change), not on every Metadata refetch -- PropertiesChanged also fires
    // for e.g. a play/pause toggle, which re-sends the same Metadata and
    // would otherwise mean decoding the same image again for no reason.
    if (strcmp(new_art_url, item->art_url) != 0) {
        snprintf(item->art_url, sizeof(item->art_url), "%s", new_art_url);
        if (item->art_surface) {
            cairo_surface_destroy(item->art_surface);
            item->art_surface = NULL;
        }
        if (item->art_url[0]) {
            item->art_surface = mpris_art_load(item->art_url, MPRIS_ART_SIZE);
        }
    }
}

// Re-fetches every Player-interface property this module cares about (a full
// GetAll, not an incremental parse of PropertiesChanged's own changed-
// properties payload) plus the root interface's Identity -- simpler and more
// robust than tracking partial updates, at the cost of one extra round trip
// per change. Acceptable here since PropertiesChanged fires relatively
// rarely (track changes, play/pause, not a hot path), same tradeoff
// dbus/sni_registry.c's refresh_icon() already makes for NewIcon.
static void fetch_player_properties(MprisPlayer *item) {
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

    if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
        while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
            const char *key = NULL;
            sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &key);

            char type = 0;
            const char *contents = NULL;
            sd_bus_message_peek_type(reply, &type, &contents);

            if (contents && sd_bus_message_enter_container(reply, SD_BUS_TYPE_VARIANT, contents) > 0) {
                if (key && strcmp(key, "PlaybackStatus") == 0 && strcmp(contents, "s") == 0) {
                    const char *status = NULL;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &status);
                    if (status) {
                        snprintf(item->status, sizeof(item->status), "%s", status);
                    }
                } else if (key && strcmp(key, "Metadata") == 0 && strcmp(contents, "a{sv}") == 0) {
                    parse_metadata(reply, item);
                } else if (key && strcmp(key, "CanGoNext") == 0 && strcmp(contents, "b") == 0) {
                    int v = 0;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_BOOLEAN, &v);
                    item->can_go_next = v != 0;
                } else if (key && strcmp(key, "CanGoPrevious") == 0 && strcmp(contents, "b") == 0) {
                    int v = 0;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_BOOLEAN, &v);
                    item->can_go_previous = v != 0;
                } else {
                    sd_bus_message_skip(reply, contents);
                }
                sd_bus_message_exit_container(reply); // variant
            }
            sd_bus_message_exit_container(reply); // dict entry
        }
        sd_bus_message_exit_container(reply); // array
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

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
        fetch_player_properties(item);
        if (g_app) {
            output_mark_all_dirty(g_app);
        }
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

    fetch_player_properties(item);

    // member=NULL subscribes to every signal from the Properties interface at
    // this player's object path (in practice, only PropertiesChanged) --
    // same "match broadly, dispatch by name inside the callback" convention
    // as dbus/sni_registry.c's sni_registry_add(). bus_name here is the
    // well-known "org.mpris.MediaPlayer2.*" name, not a unique connection
    // name -- confirmed to work as a match-rule sender the same way
    // sni_registry.c already relies on for its own per-item subscriptions.
    sd_bus_match_signal(bus, &item->signal_slot, item->bus_name, MPRIS_OBJECT_PATH,
                         "org.freedesktop.DBus.Properties", NULL, on_player_signal, item);

    if (g_app) {
        output_mark_all_dirty(g_app);
    }
}

static void remove_player(const char *bus_name) {
    MprisPlayer *item = find_player(bus_name);
    if (!item) {
        return;
    }
    if (item->signal_slot) {
        sd_bus_slot_unref(item->signal_slot);
    }
    if (item->art_surface) {
        cairo_surface_destroy(item->art_surface);
    }
    memset(item, 0, sizeof(*item)); // in_use = false
    if (g_app) {
        output_mark_all_dirty(g_app);
    }
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
