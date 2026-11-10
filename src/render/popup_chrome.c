#include "render/popup_chrome.h"

#include "render/module_box.h"
#include "render/popup_style.h"

#define HEADER_TEXT_BUF 256

void popup_draw_frame(cairo_t *cr, double w, double h, Color accent) {
    module_box_draw_rect(cr, 0, 0, w, h, COLORS.mantle, accent, (double)BAR.module_border_width);
}

double popup_pill_width(const char *word) {
    return module_box_text_width(word, FONT.xs, true) + POPUP_PILL_PAD_X * 2.0;
}

void popup_draw_pill(cairo_t *cr, double right_x, double row_y, double row_h, const char *word, Color fill) {
    double w = popup_pill_width(word);
    double x = right_x - w;
    double y = row_y + (row_h - POPUP_PILL_H) / 2.0;
    module_box_draw_rect(cr, x, y, w, POPUP_PILL_H, fill, COLORS.mantle, 0.0);
    module_box_draw_text_centered(cr, x, y, w, POPUP_PILL_H, word, FONT.xs, true, COLORS.mantle);
}

static bool has(const char *s) {
    return s && s[0];
}

void popup_draw_header(cairo_t *cr, double x, double y, double w, const PopupHeader *header) {
    double right = x + w;
    if (has(header->pill)) {
        popup_draw_pill(cr, right, y, POPUP_HEADER_H, header->pill, header->pill_fill);
        right -= popup_pill_width(header->pill) + POPUP_COL_GAP;
    }

    double tx = x;
    if (has(header->icon)) {
        module_box_draw_text_left(cr, tx, y, POPUP_HEADER_H, header->icon, FONT.heading, false, header->icon_color);
        tx += module_box_text_width(header->icon, FONT.heading, false) + POPUP_HEADER_ICON_GAP;
    }
    char buf[HEADER_TEXT_BUF];
    if (has(header->title)) {
        module_box_truncate_to_width(header->title, right - tx, FONT.md, true, buf, sizeof(buf));
        module_box_draw_text_left(cr, tx, y, POPUP_HEADER_H, buf, FONT.md, true, COLORS.text);
        tx += module_box_text_width(buf, FONT.md, true);
    }
    if (has(header->summary) && right - tx > 0.0) {
        module_box_truncate_to_width(header->summary, right - tx, FONT.sm, false, buf, sizeof(buf));
        module_box_draw_text_left(cr, tx, y, POPUP_HEADER_H, buf, FONT.sm, false, COLORS.subtext0);
    }
}

double popup_header_width(const PopupHeader *header) {
    double w = 0.0;
    if (has(header->icon)) {
        w += module_box_text_width(header->icon, FONT.heading, false) + POPUP_HEADER_ICON_GAP;
    }
    if (has(header->title)) {
        w += module_box_text_width(header->title, FONT.md, true);
    }
    if (has(header->summary)) {
        w += module_box_text_width(header->summary, FONT.sm, false);
    }
    if (has(header->pill)) {
        w += POPUP_PILL_GAP + popup_pill_width(header->pill);
    }
    return w;
}

void popup_draw_card(cairo_t *cr, double x, double y, double w, double h, bool current, bool hovered, Color accent) {
    module_box_draw_rect(cr, x, y, w, h, hovered ? COLORS.surface0 : COLORS.base, current ? accent : COLORS.surface1,
                         current ? 2.0 : 1.0);
}
