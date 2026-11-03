#ifndef CUSTOMBAR_MODULES_CLOCK_CLOCK_CALENDAR_POPUP_H
#define CUSTOMBAR_MODULES_CLOCK_CLOCK_CALENDAR_POPUP_H

#include <stdbool.h>
#include <stdint.h>

#include "wayland/context.h"

// Opens a 12-month year-calendar popup anchored under the clock module's box
// (today highlighted, past days faded, ISO week numbers, the current month's
// card outlined), or closes it if it's already open -- always opening on the
// current year; the header's arrows then flip years --
// mirrors the reference ClockWidget.qml's `yearOpen = !yearOpen` toggle on
// click, translated to this project's established right-click-opens-dropdown
// convention (see CLAUDE.md's "System monitor (CPU/GPU/memory)" section for
// why right-click, not left, is this project's answer to a module with no
// obvious primary left-click action). `box_x/y/w/h` is the clock box's own
// screen-space rect, as recorded by clock_render(). Right-anchored the same
// way modules/tray/tray_context_menu.c is, since the clock lives in the bar's
// right region, not the left (unlike sysmon/sysmon_info_popup.c's popups).
void clock_calendar_popup_toggle(Output *output, double box_x, double box_y, double box_w, double box_h);

void clock_calendar_popup_close(void);

// Left click in the popup's header: the "<" / ">" chevrons show the previous /
// next year, and the year/date text in the middle jumps back to the current
// year. Clicks anywhere else on the popup do nothing (it stays open). x/y are
// popup-surface-local.
void clock_calendar_popup_handle_click(double x, double y, uint32_t button);

// Hover highlight for those same three header targets; repaints only when the
// hovered target changes. Pass negative coordinates to clear it (pointer left).
void clock_calendar_popup_handle_motion(double x, double y);

bool clock_calendar_popup_is_menu_surface(struct wl_surface *surface);
bool clock_calendar_popup_is_catcher_surface(struct wl_surface *surface);

#endif
