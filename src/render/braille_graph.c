#include "render/braille_graph.h"

#include <math.h>
#include <stdio.h>

// Braille cell geometry: 2x4 dots per cell, DOT_PITCH apart within a cell
// and CELL_GAP extra between cells -- the extra gap is what makes it read
// as characters in a terminal rather than a plain dot matrix.
#define DOTS_X 2
#define DOTS_Y 4
#define DOT_PITCH 3.0
#define CELL_GAP 2.0
#define DOT_RADIUS 1.0

static double cell_w(void) { return DOTS_X * DOT_PITCH + CELL_GAP; }
static double cell_h(void) { return DOTS_Y * DOT_PITCH + CELL_GAP; }

double braille_graph_width(int cells_w) { return cells_w * cell_w(); }
double braille_graph_height(int cells_h) { return cells_h * cell_h(); }

int braille_graph_cells_for_width(double width) {
    int cells = (int)floor(width / cell_w());
    return cells > 0 ? cells : 0;
}

void braille_graph_span_label(char *out, size_t out_size, int cells_w, double seconds_per_sample) {
    double seconds = cells_w * DOTS_X * seconds_per_sample;
    if (seconds < 90.0) {
        snprintf(out, out_size, "%.0f s ago", seconds);
    } else {
        snprintf(out, out_size, "%.0f min ago", seconds / 60.0);
    }
}

// Offset of dot `index` (counted along one axis, across all cells) from the
// start of that axis: whole cells first, then the dot's place in its cell.
// CELL_GAP / 2 sits on each side of a cell so the gaps are symmetric.
static double dot_offset(int index, int dots_per_cell, double cell_size) {
    int cell = index / dots_per_cell;
    int sub = index % dots_per_cell;
    return cell * cell_size + CELL_GAP / 2.0 + sub * DOT_PITCH + DOT_PITCH / 2.0;
}

static Color gradient_at(const Color gradient[3], double t) {
    const Color *a = t < 0.5 ? &gradient[0] : &gradient[1];
    const Color *b = t < 0.5 ? &gradient[1] : &gradient[2];
    double f = t < 0.5 ? t * 2.0 : (t - 0.5) * 2.0;
    Color c = {
        a->r + (b->r - a->r) * f,
        a->g + (b->g - a->g) * f,
        a->b + (b->b - a->b) * f,
        a->a + (b->a - a->a) * f,
    };
    return c;
}

void braille_graph_draw(cairo_t *cr, double x, double y, int cells_w, int cells_h,
                        const double *samples, int n, double scale, bool grow_up,
                        const Color gradient[3]) {
    int columns = cells_w * DOTS_X;
    int rows = cells_h * DOTS_Y;
    double height = braille_graph_height(cells_h);
    if (columns <= 0 || rows <= 0 || scale <= 0.0) {
        return;
    }

    cairo_save(cr);
    for (int col = 0; col < columns; col++) {
        // Right-align the samples: the last column always holds the newest.
        int sample = n - columns + col;
        if (sample < 0) {
            continue;
        }
        double v = samples[sample];
        if (v <= 0.0) {
            continue;
        }
        int level = (int)lround(v / scale * rows);
        if (level < 1) {
            level = 1;
        }
        if (level > rows) {
            level = rows;
        }

        double cx = x + dot_offset(col, DOTS_X, cell_w());
        for (int r = 0; r < level; r++) {
            // Level 0 is the row at the graph's base, which is the bottom
            // edge when growing up and the top edge when hanging down.
            double from_base = dot_offset(r, DOTS_Y, cell_h());
            double cy = grow_up ? y + height - from_base : y + from_base;
            Color c = gradient_at(gradient, rows > 1 ? (double)r / (rows - 1) : 0.0);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, DOT_RADIUS, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

// --- Sprite path ---

int braille_graph_rows(int cells_h) {
    return cells_h * DOTS_Y;
}

cairo_surface_t *braille_graph_render_lit(int cells_w, int cells_h, bool grow_up, const Color gradient[3]) {
    int w = (int)ceil(braille_graph_width(cells_w)), h = (int)ceil(braille_graph_height(cells_h));
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w > 0 ? w : 1, h > 0 ? h : 1);
    cairo_t *cr = cairo_create(surf);
    int columns = cells_w * DOTS_X, rows = cells_h * DOTS_Y;
    for (int r = 0; r < rows; r++) {
        double from_base = dot_offset(r, DOTS_Y, cell_h());
        double cy = grow_up ? h - from_base : from_base;
        Color c = gradient_at(gradient, rows > 1 ? (double)r / (rows - 1) : 0.0);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
        for (int col = 0; col < columns; col++) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, dot_offset(col, DOTS_X, cell_w()), cy, DOT_RADIUS, 0, 2 * M_PI);
        }
        cairo_fill(cr); // one fill per row: a row shares its colour
    }
    cairo_destroy(cr);
    return surf;
}

void braille_graph_column_rect(int cells_h, int col, int row_from, int row_to, bool grow_up, int *x, int *y,
                               int *w, int *h) {
    double height = braille_graph_height(cells_h);
    double cx = dot_offset(col, DOTS_X, cell_w());
    // Rows' extents along the axis from the base edge: each dot's centre
    // +-DOT_PITCH/2, so the band covers whole dots and none of the next. Dot
    // centres sit on half pixels with a whole-pixel pitch, so every edge here
    // is a whole pixel.
    double near = dot_offset(row_from, DOTS_Y, cell_h()) - DOT_PITCH / 2.0;
    double far = dot_offset(row_to - 1, DOTS_Y, cell_h()) + DOT_PITCH / 2.0;
    *x = (int)lround(cx - DOT_PITCH / 2.0);
    *y = (int)lround(grow_up ? height - far : near);
    *w = (int)DOT_PITCH;
    *h = (int)lround(far - near);
}
