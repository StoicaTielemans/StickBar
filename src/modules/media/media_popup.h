#ifndef CUSTOMBAR_MODULES_MEDIA_MEDIA_POPUP_H
#define CUSTOMBAR_MODULES_MEDIA_MEDIA_POPUP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "config/config.h"
#include "wayland/context.h"

// Opens a dropdown listing every currently-tracked MPRIS player (not just
// the active one media.c renders in the bar) with prev/play-pause/next
// controls per row. Structurally the media-domain twin of
// audio/device_popup.c (same popup/click-catcher/outside-click-dismiss
// shape, same live "recompute on every relevant update while open" model --
// see media_popup_refresh_if_open() below) rather than
// the read-only CPU/GPU/memory popups, since this one is
// interactive. Anchored TOP+LEFT with box_x as the left_margin, same
// left-anchored math as modules/cpu/cpu_popup.c -- chosen because the
// media widget lives in the bar's *center* region rather than flush against
// either edge, so growing the popup rightward from the trigger's left edge
// (rather than the tray/audio popups' own right-anchored growth) was judged
// the more reasonable default; no overflow-past-either-edge clamping is
// added, same documented limitation as every other popup in this bar.
void media_popup_open(Output *output, Color accent, double box_x, double box_y, double box_w, double box_h);
void media_popup_close(void);

bool media_popup_is_menu_surface(struct wl_surface *surface);
bool media_popup_is_catcher_surface(struct wl_surface *surface);

void media_popup_handle_motion(double x, double y);

// Landing on a row's prev/play-pause/next icon dispatches the matching MPRIS
// call for that row's own player; landing on the row's track text is a
// no-op. Deliberately doesn't close the popup afterward, matching
// audio/device_popup.c's own device_popup_handle_press()'s "stay open" choice
// -- a control click here isn't a selection that should dismiss the menu.
void media_popup_handle_click(double x, double y);

// Called after any MPRIS PropertiesChanged/NameOwnerChanged update (see
// main.c's D-Bus pump) so a live track change, play/pause toggle, or a
// player appearing/disappearing shows up immediately while the popup is
// open -- same "recompute unconditionally, gated internally by
// g_popup.open" pattern as audio/device_popup.c's
// device_popup_refresh_if_open().
void media_popup_refresh_if_open(void);

// Re-fetches every visible row's playback position (MPRIS's Position isn't
// push-notified, unlike everything else this popup shows) and redraws --
// a no-op unless the popup is open. Called once a second from main.c's
// existing clock timerfd handler, reusing that already-ticking timer rather
// than adding a new poll source just for this.
void media_popup_tick(void);

// Turns each card's capture slot on (its player is playing, the dropdown is
// open) or off -- visualizer_set_source() per slot. Cheap and idempotent:
// main.c calls it once per event-loop iteration, so play/pause, players
// coming and going and the dropdown closing all take effect without each
// path having to remember to. Closed dropdown -> every slot off.
void media_popup_sync_visualizer(void);

// The PipeWire graph changed (main.c's pipewire notify fd): re-match each
// shown card's player to its playback stream -- streams come and go per
// track in browsers. A no-op when closed.
void media_popup_streams_changed(void);

// Redraws the graphs of the slots in `changed_mask` with the visualizer's
// latest levels, each on its own subsurface (bar/strip_surface.c) -- the
// popup itself isn't redrawn. A graph whose dots didn't change isn't
// committed. Called from main.c after visualizer_pump().
void media_popup_visualizer_frame(uint32_t changed_mask);

// Masks (bit = slot) of graphs currently shown, and of those that could take
// a new frame now (see strip_surface_ready()) -- visualizer_pump()'s inputs.
void media_popup_visualizer_status(uint32_t *shown_mask, uint32_t *ready_mask);

// Test-control hooks (testing/test_control.c): cards shown (-1 if closed),
// and the popup-local centre of a player's "prev" / "play" / "next" chip,
// matched by mpris_player_app_name(). Not used by the bar itself.
int media_popup_test_card_count(void);

// Card `card`'s spectrum graph, popup-local px. False if closed or that card
// has no graph. Dot geometry inside a half is render/braille_graph.c's (8x14px
// cells, 3px dot pitch, 1px half-gap at each cell edge).
typedef struct {
    char app[64];          // the card's player, as mpris_player_app_name() gives it
    int slot;              // its visualizer slot
    uint32_t target;       // captured stream's object.serial, 0 = output mix
    double panel_x, panel_y, panel_w, panel_h; // the panel = the strip subsurface
    double graph_x;        // left edge of the dot graph
    double top_y;          // top of the L half (its dots grow up from top_y + half_h)
    double bottom_y;       // top of the R half (its dots hang down from here)
    double half_h;
    int rows;              // dot rows per half
} MediaPopupTestGraph;
bool media_popup_test_graph(int card, MediaPopupTestGraph *out);
// Where to click on `app_name`'s card: `button` is "prev", "next", "play", or
// "timeline:<fraction>" for that far along its timeline meter (at the row's
// vertical centre).
bool media_popup_test_target(const char *app_name, const char *button, double *x, double *y);

#endif
