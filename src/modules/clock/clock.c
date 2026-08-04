#include "modules/clock/clock.h"

#include <stdio.h>
#include <time.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "render/module_box.h"

// Cached formatted string, e.g. "󰃮 04/08/26 󰥔 14:03:07" -- refreshed once per
// second by clock_tick() (driven from main.c's timerfd), read by clock_render() on
// every frame without reformatting.
static char g_time_str[64] = "";

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

double clock_render(cairo_t *cr, double right_edge_x, double box_top, double box_height) {
    if (g_time_str[0] == '\0') {
        // Guards against rendering before the first clock_tick() (main.c calls it
        // once at startup before the event loop begins, but this keeps the
        // function safe to call standalone regardless).
        clock_tick();
    }

    double text_w, text_h;
    module_box_measure_text(g_time_str, FONT.label, true, &text_w, &text_h);

    // Matches the reference ClockWidget.qml's `implicitWidth: label.implicitWidth + 12`.
    double box_w = text_w + 12.0;
    double box_x = right_edge_x - box_w;

    module_box_draw_rect(cr, box_x, box_top, box_w, box_height,
                          COLORS.mantle, COLORS.mauve, (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, box_x, box_top, box_w, box_height,
                                   g_time_str, FONT.label, true, COLORS.mauve);

    (void)text_h;
    return box_x;
}
