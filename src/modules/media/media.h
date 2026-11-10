#ifndef CUSTOMBAR_MODULES_MEDIA_MEDIA_H
#define CUSTOMBAR_MODULES_MEDIA_MEDIA_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mpris/mpris_client.h"
#include "wayland/context.h"

// The title to show for `player`: its track title, else its own identity
// (some players report no title), else "Unknown" -- matches MediaWidget.qml's
// trackText() fallback. Shared by the bar box and the dropdown cards.
const char *media_display_title(MprisPlayer *player);

// Draws the active MPRIS player's module box horizontally centered within a
// `bar_width`-wide bar, vertically spanning box_top..box_top+box_height --
// matches the reference bar's own centerLayout, which holds only this one
// widget (`anchors.horizontalCenter: parent.horizontalCenter`). Draws
// nothing, and clears the box's hit-rect to zero size, when there's no
// active MPRIS player at all -- matches MediaWidget.qml's own
// `visible: player !== null` collapsing it to zero width rather than showing
// an empty box. Records the box's screen-space rect for later
// media_handle_click().
//
// `left_limit`/`right_limit` are where the left region's modules end and the
// right region's begin (spacing already included). The box stays centered but
// its track text is truncated further so the box never crosses either limit;
// if not even the icon plus "..." fits, nothing is drawn (same zero-size
// hit-rect as the no-player case), so it can't steal a neighbor's clicks.
// The box's rect as last drawn on `output`; false if nothing was drawn there
// (no player, or collapsed for lack of room). Same shape as clock_last_rect().
bool media_last_rect(Output *output, double *x, double *y, double *w, double *h);

void media_render(cairo_t *cr, Output *output, double bar_width, double left_limit, double right_limit,
                  double box_top, double box_height);

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
