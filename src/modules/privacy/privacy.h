#ifndef CUSTOMBAR_MODULES_PRIVACY_PRIVACY_H
#define CUSTOMBAR_MODULES_PRIVACY_PRIVACY_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the privacy indicator box left-anchored so its left edge sits at `x`,
// vertically spanning box_top..box_top+box_height -- same left-growing shape
// as cpu_render()/gpu_render()/mem_render(), since this lives in the bar's
// left region too. Unlike those three, this module collapses to zero width
// (same convention modules/media/media.c uses for "nothing to show") when
// pipewire_client_any_active() is false: no box is drawn and `x` is returned
// unadvanced, so the next left-region module starts exactly where this one
// would have. When at least one of mic/webcam/screen-share is active, draws
// one box containing the glyph for each active category and records the
// box's screen-space rect for later privacy_handle_click(). Returns `x`
// advanced past the box plus BAR.item_spacing (or unchanged if collapsed).
double privacy_render(cairo_t *cr, Output *output, double x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box (a no-op if
// collapsed, since the recorded rect's w/h are zeroed in that case too).
// Right-click opens a read-only dropdown listing which app is using which
// category, via privacy_info_popup.c; left-click is a no-op -- same "no
// obvious single primary action" convention as cpu/gpu/mem/network.
void privacy_handle_click(Output *output, double x, double y, uint32_t button);

#endif
