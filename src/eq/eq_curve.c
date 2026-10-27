#include "eq/eq_curve.h"

#include <math.h>

#include "eq/eq_state.h"

// Matches the script's own curve span (CURVE_FMIN/CURVE_FMAX in
// blackpearl-eq.py) -- the curve's points are log-spaced across exactly this.
#define FREQ_MIN 20.0
#define FREQ_MAX 20000.0

double eq_curve_range_db(double min_range, double step) {
    double extent = fabs(eq_state_peak_db());
    if (fabs(eq_state_min_db()) > extent) {
        extent = fabs(eq_state_min_db());
    }
    double range = ceil(extent / step) * step;
    return range < min_range ? min_range : range;
}

double eq_curve_freq_to_x(double freq, double x, double w) {
    if (freq < FREQ_MIN) freq = FREQ_MIN;
    if (freq > FREQ_MAX) freq = FREQ_MAX;
    return x + w * log(freq / FREQ_MIN) / log(FREQ_MAX / FREQ_MIN);
}

void eq_curve_draw(cairo_t *cr, double x, double y, double w, double h, double range_db,
                   Color color, double line_width, bool zero_line, Color zero_color) {
    double mid_y = y + h / 2.0;
    double px_per_db = (h / 2.0) / range_db;

    cairo_save(cr);
    // Clip to the rect so a curve point beyond +/-range_db (only possible if
    // a caller passes a range smaller than the curve's own extent) can't
    // draw over the surrounding box border.
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    if (zero_line) {
        cairo_set_source_rgba(cr, zero_color.r, zero_color.g, zero_color.b, zero_color.a);
        cairo_set_line_width(cr, 1.0);
        // +0.5 so a 1px line lands on a pixel row instead of straddling two
        // (cairo's coordinates are pixel *edges*, not pixel centers).
        cairo_move_to(cr, x, floor(mid_y) + 0.5);
        cairo_line_to(cr, x + w, floor(mid_y) + 0.5);
        cairo_stroke(cr);
    }

    int n = eq_state_curve_count();
    if (n >= 2) {
        cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
        cairo_set_line_width(cr, line_width);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (int i = 0; i < n; i++) {
            double px = eq_curve_freq_to_x(eq_state_curve_freq(i), x, w);
            double py = mid_y - eq_state_curve_db(i) * px_per_db; // +dB is up
            if (i == 0) {
                cairo_move_to(cr, px, py);
            } else {
                cairo_line_to(cr, px, py);
            }
        }
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}
