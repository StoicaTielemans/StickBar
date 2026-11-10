#include "render/render.h"

#include <string.h>

#include "config/config.h"
#include "modules/clock/clock.h"
#include "modules/cpu/cpu.h"
#include "modules/eq/eq.h"
#include "modules/gpu/gpu.h"
#include "modules/media/media.h"
#include "modules/mem/mem.h"
#include "modules/mic/mic.h"
#include "modules/network/network.h"
#include "modules/powermenu/arch_logo.h"
#include "modules/privacy/privacy.h"
#include "modules/tray/tray.h"
#include "modules/volume/volume.h"
#include "modules/workspaces/workspaces.h"
#include "render/color.h"

// Module box rects per output, for render_module_rect(). Sized like every
// other per-output table in this project (MAX_TRACKED_OUTPUTS = 8).
#define MAX_RECT_OUTPUTS 8
#define MAX_RECT_MODULES 16
typedef struct {
    const char *module; // static string, NULL = unused slot
    double x, w;        // y/h are always the whole bar height
} ModuleRect;
typedef struct {
    Output *output;
    int height;
    ModuleRect rects[MAX_RECT_MODULES];
    int count;
} OutputRects;
static OutputRects g_rects[MAX_RECT_OUTPUTS];

static OutputRects *rects_for(Output *output) {
    OutputRects *free_slot = NULL;
    for (int i = 0; i < MAX_RECT_OUTPUTS; i++) {
        if (g_rects[i].output == output) {
            return &g_rects[i];
        }
        if (!free_slot && !g_rects[i].output) {
            free_slot = &g_rects[i];
        }
    }
    if (free_slot) {
        free_slot->output = output;
    }
    return free_slot;
}

static void record(OutputRects *r, const char *module, double x, double w) {
    if (r && r->count < MAX_RECT_MODULES && w > 0.0) {
        r->rects[r->count++] = (ModuleRect){ module, x, w };
    }
}

// Every left-region render function takes its box's left x and returns the
// next module's start (its own right edge + BAR.item_spacing); right-region
// ones take their box's right x and return the next module's right edge
// (their own left edge - BAR.item_spacing). These two turn that back into a
// box. A collapsed module returns its input unchanged -> zero width -> not
// recorded.
static void record_left(OutputRects *r, const char *module, double x_in, double x_out) {
    record(r, module, x_in, x_out - BAR.item_spacing - x_in);
}
static void record_right(OutputRects *r, const char *module, double x_in, double x_out) {
    double left = x_out + BAR.item_spacing;
    record(r, module, left, x_in - left);
}

// The bar's two side regions, in drawing order. Each render function takes
// the x to start at (its left edge for the left region, its right edge for
// the right one) and returns where the next module goes; a collapsed module
// returns its input unchanged.
typedef struct {
    const char *name; // for render_module_rect() / the test-control channel
    double (*render)(cairo_t *cr, Output *output, double x, double box_top, double box_height);
} RegionModule;

// Left region: matches the reference bar's actual leftLayout order --
// ArchLogoWidget (the power-menu trigger), then HyprlandWindows
// (workspaces), then SystemMonitorWidget's CPU/GPU/MEM stats, split into
// three separate modules per explicit user request but kept in that same
// relative left-to-right order.
static const RegionModule LEFT_REGION[] = {
    {"arch", arch_logo_render}, {"workspaces", workspaces_render}, {"cpu", cpu_render},
    {"gpu", gpu_render},        {"mem", mem_render},               {"privacy", privacy_render},
};

// Right region: matches the reference Bar.qml's actual rightLayout order,
// Mic, Volume, Network, Clock, Tray -- Tray is rightmost, drawn first
// here since this works right-to-left. The DAC EQ module (no reference
// counterpart -- a StickBar-only addition) sits between Volume and Network,
// keeping all the audio widgets together.
static const RegionModule RIGHT_REGION[] = {
    {"tray", tray_render}, {"clock", clock_render}, {"network", network_render},
    {"eq", eq_render},     {"volume", volume_render}, {"mic", mic_render},
};

bool render_module_rect(Output *output, const char *module, double *x, double *y, double *w, double *h) {
    if (strcmp(module, "media") == 0) {
        return media_last_rect(output, x, y, w, h);
    }
    for (int i = 0; i < MAX_RECT_OUTPUTS; i++) {
        if (g_rects[i].output != output) {
            continue;
        }
        for (int j = 0; j < g_rects[i].count; j++) {
            if (strcmp(g_rects[i].rects[j].module, module) == 0) {
                *x = g_rects[i].rects[j].x;
                *w = g_rects[i].rects[j].w;
                *y = 0.0;
                *h = g_rects[i].height;
                return true;
            }
        }
    }
    return false;
}

void render_output(cairo_t *cr, Output *output, int width, int height) {
    OutputRects *rects = rects_for(output);
    if (rects) {
        rects->count = 0;
        rects->height = height;
    }

    // Background fill. This also implicitly erases everything from the previous
    // frame (e.g. a workspace pill that's since stopped being occupied), so no
    // separate clear/memset step is needed anywhere else in the render path.
    color_set_source(cr, COLORS.mantle);
    cairo_paint(cr);

    // Left region (see LEFT_REGION).
    double left_region_end_x = 0.0;
    for (size_t i = 0; i < sizeof(LEFT_REGION) / sizeof(LEFT_REGION[0]); i++) {
        double next_x = LEFT_REGION[i].render(cr, output, left_region_end_x, 0.0, (double)height);
        record_left(rects, LEFT_REGION[i].name, left_region_end_x, next_x);
        left_region_end_x = next_x;
    }

    // Right region, right to left (see RIGHT_REGION).
    double right_region_start_x = (double)width;
    for (size_t i = 0; i < sizeof(RIGHT_REGION) / sizeof(RIGHT_REGION[0]); i++) {
        double next_x = RIGHT_REGION[i].render(cr, output, right_region_start_x, 0.0, (double)height);
        record_right(rects, RIGHT_REGION[i].name, right_region_start_x, next_x);
        right_region_start_x = next_x;
    }

    // Center region: matches the reference bar's own centerLayout, which
    // holds only the media widget, horizontally centered
    // (`anchors.horizontalCenter: parent.horizontalCenter`) rather than
    // anchored to either edge like every other module here. Drawn last so it
    // knows where both side regions ended, and shrinks to fit between them
    // instead of drawing over them (both bounds already include
    // BAR.item_spacing, so they double as the gap).
    media_render(cr, output, (double)width, left_region_end_x, right_region_start_x, 0.0, (double)height);
}

bool render_output_clock_only(cairo_t *cr, Output *output, int width, int height) {
    (void)width;
    double x, y, w, h;
    if (!clock_last_rect(output, &x, &y, &w, &h)) {
        return false;
    }

    // Everything outside the clock's own box is left exactly as the previous
    // frame had it. Clip to the box, clear it to the bar background, redraw
    // the clock with the same right edge it had last time.
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    color_set_source(cr, COLORS.mantle);
    cairo_paint(cr);
    clock_render(cr, output, x + w, 0.0, (double)height);
    cairo_restore(cr);

    // The digits are monospaced so the width shouldn't change -- but if a
    // format change ever made it, the network/volume/... boxes to its left
    // would need to shift, which only a full render can do.
    double nx, ny, nw, nh;
    return clock_last_rect(output, &nx, &ny, &nw, &nh) && nw == w;
}
