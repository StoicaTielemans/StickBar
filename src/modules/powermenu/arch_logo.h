#ifndef CUSTOMBAR_MODULES_POWERMENU_ARCH_LOGO_H
#define CUSTOMBAR_MODULES_POWERMENU_ARCH_LOGO_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the Arch Linux logo "cutout" left-anchored so its left edge sits at
// `x`, vertically spanning box_top..box_top+box_height -- the bar's own
// leftmost item, matching the reference Bar.qml's own leftLayout order
// (ArchLogoWidget, then HyprlandWindows/workspaces). Unlike every other
// module box in this project, this one has no border -- a solid
// COLORS.sapphire fill with the Arch glyph rendered in COLORS.mantle (the
// bar's own background) so the logo reads as negative space cut out of a
// solid block, not colored text on a dark box like everything else (matches
// ArchLogoWidget.qml's own styling exactly, deliberately not reusing
// module_box's usual bordered-box look). The box is sized from the glyph's
// *ink* extent plus this project's usual +12px pad, not the reference's own
// tighter +8px-over-logical-width pad -- confirmed via a direct Pango probe
// that this specific glyph's ink is significantly wider than its own
// logical/advance width (see module_box_measure_text_ink()'s own doc
// comment), so matching the reference's exact padding here left part of the
// logo's own ink landing outside the box. Records the box's screen-space
// rect for later arch_logo_handle_click(). Returns x advanced past this box
// plus BAR.item_spacing, for the next (further right) left-region module to
// anchor against.
double arch_logo_render(cairo_t *cr, Output *output, double x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box.
// Left-click toggles the power menu overlay (modules/powermenu/power_menu.h)
// -- matches the reference's own plain `onClicked: PowerMenuState.toggle()`,
// which (per QtQuick's own MouseArea default) only ever fires for the left
// button. No-op if (x, y) doesn't land on the box.
void arch_logo_handle_click(Output *output, double x, double y, uint32_t button);

#endif
