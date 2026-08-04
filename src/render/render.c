#include "render/render.h"

#include "config/config.h"
#include "modules/clock/clock.h"
#include "modules/workspaces/workspaces.h"

void render_output(cairo_t *cr, Output *output, int width, int height) {
    // Background fill. This also implicitly erases everything from the previous
    // frame (e.g. a workspace pill that's since stopped being occupied), so no
    // separate clear/memset step is needed anywhere else in the render path.
    cairo_set_source_rgba(cr, COLORS.mantle.r, COLORS.mantle.g, COLORS.mantle.b, COLORS.mantle.a);
    cairo_paint(cr);

    // Left region: workspace pills, flush against the left edge (matches the
    // reference bar's leftLayout, which anchors to parent.left with no margin --
    // spacing only applies *between* items, not before the first one).
    workspaces_render(cr, output, 0.0, 0.0, (double)height);

    // Right region: the clock, flush against the right edge. Center region is
    // intentionally left empty -- scaffolding for a future module (media player in
    // the reference bar).
    clock_render(cr, (double)width, 0.0, (double)height);
}
