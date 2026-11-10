#include "modules/eq/eq.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>

#include "config/config.h"
#include "eq/eq_curve.h"
#include "eq/eq_state.h"
#include "modules/eq/eq_popup.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Sparkline geometry, local to this widget (see CLAUDE.md "Conventions").
// 34px wide is enough to read the shape (bass shelf vs. treble cuts) at a
// glance without the box crowding its neighbours; the vertical inset keeps
// the line clear of the box's own 2px border on a 20px bar.
#define SPARK_W 34.0
#define SPARK_INSET_Y 4.0
#define SPARK_GAP 6.0       // between the sparkline and the profile name
// The sparkline's dB span is at least +/-6 dB (so a few gentle treble cuts
// still read as gentle), growing in 6 dB steps to fit heavier EQs.
#define SPARK_MIN_RANGE_DB 6.0
#define SPARK_RANGE_STEP_DB 6.0

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

// The label next to the sparkline: the profile being applied (with an
// ellipsis) takes priority, then the active profile, then a fallback for an
// EQ that matches no preset, or for no state file at all yet.
static void build_label(char *out, size_t out_size) {
    const char *pending = eq_state_pending_profile();
    if (pending[0]) {
        snprintf(out, out_size, "%s...", pending);
    } else if (!eq_state_available()) {
        snprintf(out, out_size, "EQ --");
    } else if (eq_state_profile()[0]) {
        snprintf(out, out_size, "%s", eq_state_profile());
    } else {
        snprintf(out, out_size, "custom");
    }
}

double eq_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height) {
    char label[96];
    build_label(label, sizeof(label));

    double text_w, text_h;
    module_box_measure_text(label, FONT.label, true, &text_w, &text_h);
    (void)text_h;

    // Matches this project's other module boxes' `label + 12` padding
    // convention, with the sparkline counted as part of the label.
    double content_w = SPARK_W + SPARK_GAP + text_w;
    double box_w = content_w + 12.0;
    double box_x = right_edge_x - box_w;

    // A disabled EQ (`blackpearl-eq.py off`) still shows its curve -- it's
    // still stored on the DAC -- but greyed out, so it's obvious it isn't
    // actually being applied.
    Color accent = eq_state_enabled() ? COLORS.lavender : COLORS.overlay0;

    module_box_draw_rect(cr, box_x, box_top, box_w, box_height,
                          COLORS.mantle, accent, (double)BAR.module_border_width);

    double spark_x = box_x + 6.0;
    eq_curve_draw(cr, spark_x, box_top + SPARK_INSET_Y, SPARK_W, box_height - SPARK_INSET_Y * 2.0,
                  eq_curve_range_db(SPARK_MIN_RANGE_DB, SPARK_RANGE_STEP_DB),
                  accent, 1.5, true, COLORS.surface1);

    // Text centered within the remaining space right of the sparkline.
    double text_x = spark_x + SPARK_W + SPARK_GAP;
    module_box_draw_text_centered(cr, text_x, box_top, text_w, box_height, label, FONT.label, true, accent);

    box_rect_set(&g_rects, output, box_x, box_top, box_w, box_height);

    return box_x - BAR.item_spacing;
}

void eq_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
        return;
    }
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    eq_popup_open(output, rect->x, rect->y, rect->w, rect->h);
}
