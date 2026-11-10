#include "modules/clock/clock.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>
#include <time.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/clock/clock_calendar_popup.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Cached formatted string, e.g. "󰃮 04/08/26 󰥔 14:03:07" -- refreshed once per
// second by clock_tick() (driven from main.c's timerfd), read by clock_render() on
// every frame without reformatting.
static char g_time_str[64] = "";

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

void clock_tick(void) {
    char calendar_glyph[5];
    char clock_glyph[5];
    glyph_utf8(GLYPH_ICON_CALENDAR, calendar_glyph);
    glyph_utf8(GLYPH_ICON_CLOCK, clock_glyph);

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    // Matches the reference Time.qml's format exactly: "{cal} dd/mm/yy {clock} hh:mm:ss".
    // tm_year is years since 1900; %100 gives the last two digits the same way the
    // QML original computes `now.getFullYear() % 100`.
    snprintf(g_time_str, sizeof(g_time_str), "%s %02d/%02d/%02d %s %02d:%02d:%02d",
              calendar_glyph,
              local.tm_mday, local.tm_mon + 1, local.tm_year % 100,
              clock_glyph,
              local.tm_hour, local.tm_min, local.tm_sec);
}

double clock_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height) {
    if (g_time_str[0] == '\0') {
        // Guards against rendering before the first clock_tick() (main.c calls it
        // once at startup before the event loop begins, but this keeps the
        // function safe to call standalone regardless).
        clock_tick();
    }

    return module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_RIGHT, right_edge_x, box_top, box_height, g_time_str,
                            COLORS.mauve);
}

bool clock_last_rect(Output *output, double *x, double *y, double *w, double *h) {
    BoxRect r;
    if (!box_rect_get(&g_rects, output, &r)) {
        return false;
    }
    *x = r.x;
    *y = r.y;
    *w = r.w;
    *h = r.h;
    return true;
}

void clock_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
        return;
    }
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    clock_calendar_popup_toggle(output, rect->x, rect->y, rect->w, rect->h);
}
