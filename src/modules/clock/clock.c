#include "modules/clock/clock.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>
#include <stdlib.h> // getenv
#include <string.h>
#include <time.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/clock/clock_calendar_popup.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Cached formatted string, e.g. "󰃮 04/08/26 󰥔 14:03" -- refreshed once per
// second by clock_tick() (driven from main.c's timerfd), read by clock_render() on
// every frame without reformatting.
static char g_time_str[64] = "";

// Seconds are hidden by default: the text then changes once a minute, so the bar
// commits once a minute instead of every second (see clock_tick()'s return value).
// STICKBAR_TEST_CLOCK_SECONDS brings them back so the clock tests still get a
// per-second change to measure (tests/test_clock_alignment.py, test_redraw.py).
static bool show_seconds(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("STICKBAR_TEST_CLOCK_SECONDS") != NULL;
    }
    return cached;
}

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

bool clock_tick(void) {
    char calendar_glyph[5];
    char clock_glyph[5];
    glyph_utf8(GLYPH_ICON_CALENDAR, calendar_glyph);
    glyph_utf8(GLYPH_ICON_CLOCK, clock_glyph);

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    // The reference Time.qml's format minus the seconds: "{cal} dd/mm/yy {clock} hh:mm".
    // tm_year is years since 1900; %100 gives the last two digits the same way the
    // QML original computes `now.getFullYear() % 100`.
    char next[sizeof(g_time_str)];
    int n = snprintf(next, sizeof(next), "%s %02d/%02d/%02d %s %02d:%02d",
                     calendar_glyph,
                     local.tm_mday, local.tm_mon + 1, local.tm_year % 100,
                     clock_glyph,
                     local.tm_hour, local.tm_min);
    if (show_seconds() && n > 0 && (size_t)n < sizeof(next)) {
        snprintf(next + n, sizeof(next) - (size_t)n, ":%02d", local.tm_sec);
    }

    if (strcmp(next, g_time_str) == 0) {
        return false;
    }
    memcpy(g_time_str, next, sizeof(g_time_str));
    return true;
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
