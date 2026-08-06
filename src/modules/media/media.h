#ifndef CUSTOMBAR_MODULES_MEDIA_MEDIA_H
#define CUSTOMBAR_MODULES_MEDIA_MEDIA_H

#include <cairo.h>
#include <stddef.h>
#include <stdint.h>

#include "mpris/mpris_client.h"
#include "wayland/context.h"

// Formats `player`'s "artist - title" display text (falling back to its own
// identity, then "Unknown", when no title is reported) -- matches
// MediaWidget.qml's trackText(). Shared with modules/media/media_popup.c so
// both the bar widget and its dropdown format a player's track text
// identically, unlike the byte-based ellipsis truncation each applies
// afterward (kept as separate local copies -- see media.c's own comment on
// why that one is duplicated instead).
void media_build_track_text(MprisPlayer *player, char *out, size_t out_size);

// Draws the active MPRIS player's module box horizontally centered within a
// `bar_width`-wide bar, vertically spanning box_top..box_top+box_height --
// matches the reference bar's own centerLayout, which holds only this one
// widget (`anchors.horizontalCenter: parent.horizontalCenter`). Draws
// nothing, and clears the box's hit-rect to zero size, when there's no
// active MPRIS player at all -- matches MediaWidget.qml's own
// `visible: player !== null` collapsing it to zero width rather than showing
// an empty box. Records the box's screen-space rect for later
// media_handle_click().
void media_render(cairo_t *cr, Output *output, double bar_width, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Left-click
// toggles play/pause on the active player, matching the reference's own
// onClicked. Right-click opens a dropdown listing every tracked player (not
// just the active one) with prev/play-pause/next controls
// (modules/media/media_popup.c) -- this project's now-established
// right-click-opens-dropdown convention (see volume/mic/cpu/gpu/mem/clock),
// replacing the reference's hover-triggered MediaPopup since
// wayland/pointer.c has no hover-duration/timer infrastructure (same
// reasoning as every other popup in this bar). No-op if (x, y) doesn't land
// on the box, or if there's no active player (the box has zero size then).
void media_handle_click(Output *output, double x, double y, uint32_t button);

#endif
