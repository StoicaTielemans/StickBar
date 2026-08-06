#ifndef CUSTOMBAR_MPRIS_MPRIS_CLIENT_H
#define CUSTOMBAR_MPRIS_MPRIS_CLIENT_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-bus.h>

#include "wayland/context.h"

typedef struct MprisPlayer MprisPlayer; // opaque

// Discovers every currently-running MPRIS player on the session bus (any
// "org.mpris.MediaPlayer2.*" bus name, excluding playerctld -- see
// mpris_client.c's add_player() for why) and keeps a live cache of each
// one's PlaybackStatus/Metadata/capability properties, updated via that
// player's own PropertiesChanged signal plus NameOwnerChanged for players
// appearing/disappearing entirely. Fully reactive, no polling -- matches the
// reference MediaPlayer.qml's own Quickshell.Services.Mpris-backed singleton.
// `app` is stashed for output_mark_all_dirty() on any change, same
// convention as dbus/sni_registry.c's sni_registry_init(). Call once, after
// the shared sd_bus session-bus connection (dbus/dbus_loop.c) is up -- this
// module reuses that same connection rather than opening a second one, since
// it's talking to the same session bus the tray already does.
void mpris_client_init(sd_bus *bus, AppContext *app);

// The "active" player, matching MediaPlayer.qml's pickActive(): the first
// currently-Playing tracked player, or the first tracked player at all if
// none are playing, or NULL if there are none.
MprisPlayer *mpris_active_player(void);

int mpris_player_count(void);
MprisPlayer *mpris_player_get(int index); // 0 <= index < mpris_player_count()

const char *mpris_player_title(MprisPlayer *player);    // "" if unreported
const char *mpris_player_artist(MprisPlayer *player);   // "" if unreported
const char *mpris_player_identity(MprisPlayer *player); // app-provided display name, used as a title fallback
bool mpris_player_is_playing(MprisPlayer *player);
bool mpris_player_can_go_next(MprisPlayer *player);
bool mpris_player_can_go_previous(MprisPlayer *player);

// Track length, in microseconds, from the last-fetched Metadata's
// "mpris:length" -- 0 if unreported (a track's length is optional per spec;
// treat 0 the same as "unknown", matching the reference MediaPopup.qml's own
// hasDuration() check).
int64_t mpris_player_length_us(MprisPlayer *player);

// Playback position, in microseconds, as of the last mpris_player_refresh_position()
// call -- NOT kept live automatically, since MPRIS's Position property isn't
// push-notified via PropertiesChanged per spec (a player is expected to be
// polled for it while something needs to show it moving). 0 if never
// refreshed.
int64_t mpris_player_position_us(MprisPlayer *player);

// Synchronously re-fetches just the Position property for `player` (one
// sd_bus_get_property() round trip) and updates the cached value
// mpris_player_position_us() returns. Callers (modules/media/media_popup.c)
// are expected to call this on a timer while showing a live position, same
// "poll only while something's actually watching it" idea as the reference
// MediaPopup.qml's own `Timer { running: root.visible }` re-triggering
// positionChanged() -- see CLAUDE.md's "Media (MPRIS)" section.
void mpris_player_refresh_position(MprisPlayer *player);

// Album art for the currently-cached track, already decoded and scaled to a
// fixed thumbnail size at fetch time (mpris/mpris_art.c) -- NULL if the
// player reported no mpris:artUrl, the URL uses an unsupported scheme
// (anything other than file://), or decoding failed. Re-decoded only when
// Metadata's own artUrl string actually changes (i.e. on a real track
// change), not on every property refresh.
cairo_surface_t *mpris_player_art_surface(MprisPlayer *player);

// Fire-and-forget MPRIS method calls -- same sd_bus_send()-not-
// sd_bus_call_method() reasoning as dbus/sni_registry.c's
// sni_item_activate(): these run from a UI click handler inside the bar's
// single-threaded event loop and must never block waiting on a remote
// player's reply. mpris_player_next()/_previous() no-op if the player's own
// CanGoNext/CanGoPrevious says it can't; mpris_player_toggle_playing() always
// sends PlayPause unconditionally (spec-optional but implemented by
// essentially every real player, unlike the reference's own
// canTogglePlaying-gated Play()/Pause() fallback, which wasn't judged worth
// replicating for v1).
void mpris_player_toggle_playing(MprisPlayer *player);
void mpris_player_next(MprisPlayer *player);
void mpris_player_previous(MprisPlayer *player);

#endif
