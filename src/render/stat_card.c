#include "render/stat_card.h"

#include "render/braille_graph.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "render/popup_style.h"

#define AXIS_OFFSET 2.0         // card bottom -> axis text
#define SECONDS_PER_SAMPLE 5.0  // the 5s sysmon/network poll
#define APP_BAR_SEGMENTS 7

int stat_graph_cells(double card_w, int history_len) {
    int cells = braille_graph_cells_for_width(card_w - STAT_CARD_PAD * 2.0);
    return cells < history_len / 2 ? cells : history_len / 2;
}

int stat_history_window(int history_len, int cells, int *n) {
    int first = history_len - cells * 2;
    if (first < 0) {
        first = 0;
    }
    *n = history_len - first;
    return first;
}

void stat_graph_label_row(cairo_t *cr, double x, double y, double w, const char *left, Color left_color,
                          const char *right) {
    module_box_draw_text_left(cr, x, y, STAT_GRAPH_LABEL_H, left, FONT.sm, true, left_color);
    module_box_draw_text_right(cr, x + w, y, STAT_GRAPH_LABEL_H, right, FONT.xs, false, COLORS.overlay0);
}

double stat_graph_axis(cairo_t *cr, double x, double y, double w, int cells) {
    if (cr) {
        char span[24];
        braille_graph_span_label(span, sizeof(span), cells, SECONDS_PER_SAMPLE);
        module_box_draw_text_left(cr, x, y + AXIS_OFFSET, STAT_AXIS_H, span, FONT.xs, false, COLORS.overlay0);
        module_box_draw_text_right(cr, x + w, y + AXIS_OFFSET, STAT_AXIS_H, "now", FONT.xs, false, COLORS.overlay0);
    }
    return STAT_AXIS_H + AXIS_OFFSET;
}

double stat_label_col_w(const char *const *labels, size_t count) {
    double w = 0.0;
    for (size_t i = 0; i < count; i++) {
        double lw = module_box_text_width(labels[i], FONT.xs, true);
        if (lw > w) {
            w = lw;
        }
    }
    return w;
}

void stat_draw_kv_row(cairo_t *cr, double x, double value_x, double y, const char *label, const char *value) {
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, label, FONT.xs, true, COLORS.overlay0);
    module_box_draw_text_left(cr, value_x, y, STAT_ROW_H, value, FONT.label, false, COLORS.text);
}

double stat_app_name_col_w(void) {
    return module_box_text_width("000000000000000", FONT.label, false) + POPUP_COL_GAP;
}

void stat_draw_app_row(cairo_t *cr, double x, double y, double w, const char *name, double fraction,
                       Color bar_color, const char *value) {
    const Color gradient[3] = {bar_color, bar_color, bar_color};
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, name, FONT.label, false, COLORS.text);
    meter_bar_draw(cr, x + stat_app_name_col_w(), y + (STAT_ROW_H - STAT_APP_BAR_H) / 2.0, STAT_APP_BAR_W,
                   STAT_APP_BAR_H, fraction, APP_BAR_SEGMENTS, gradient, COLORS.surface0);
    module_box_draw_text_right(cr, x + w, y, STAT_ROW_H, value, FONT.label, false, COLORS.subtext0);
}
