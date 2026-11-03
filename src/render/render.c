#include "render/render.h"

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

void render_output(cairo_t *cr, Output *output, int width, int height) {
    // Background fill. This also implicitly erases everything from the previous
    // frame (e.g. a workspace pill that's since stopped being occupied), so no
    // separate clear/memset step is needed anywhere else in the render path.
    cairo_set_source_rgba(cr, COLORS.mantle.r, COLORS.mantle.g, COLORS.mantle.b, COLORS.mantle.a);
    cairo_paint(cr);

    // Left region: matches the reference bar's actual leftLayout order --
    // ArchLogoWidget (the power-menu trigger), then HyprlandWindows
    // (workspaces), then SystemMonitorWidget's CPU/GPU/MEM stats, split into
    // three separate modules per explicit user request but kept in that same
    // relative left-to-right order.
    double arch_logo_right_x = arch_logo_render(cr, output, 0.0, 0.0, (double)height);
    double workspaces_right_x = workspaces_render(cr, output, arch_logo_right_x, 0.0, (double)height);
    double cpu_right_x = cpu_render(cr, output, workspaces_right_x, 0.0, (double)height);
    double gpu_right_x = gpu_render(cr, output, cpu_right_x, 0.0, (double)height);
    double mem_right_x = mem_render(cr, output, gpu_right_x, 0.0, (double)height);
    double left_region_end_x = privacy_render(cr, output, mem_right_x, 0.0, (double)height);

    // Right region: matches the reference Bar.qml's actual rightLayout order,
    // Mic, Volume, Network, Clock, Tray -- Tray is rightmost, drawn first
    // here since this loop works right-to-left. The DAC EQ module (no
    // reference counterpart -- a StickBar-only addition) sits between Volume
    // and Network, keeping all the audio widgets together.
    double tray_left_x = tray_render(cr, output, (double)width, 0.0, (double)height);
    double clock_left_x = clock_render(cr, output, tray_left_x, 0.0, (double)height);
    double network_left_x = network_render(cr, output, clock_left_x, 0.0, (double)height);
    double eq_left_x = eq_render(cr, output, network_left_x, 0.0, (double)height);
    double volume_left_x = volume_render(cr, output, eq_left_x, 0.0, (double)height);
    double right_region_start_x = mic_render(cr, output, volume_left_x, 0.0, (double)height);

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
    cairo_set_source_rgba(cr, COLORS.mantle.r, COLORS.mantle.g, COLORS.mantle.b, COLORS.mantle.a);
    cairo_paint(cr);
    clock_render(cr, output, x + w, 0.0, (double)height);
    cairo_restore(cr);

    // The digits are monospaced so the width shouldn't change -- but if a
    // format change ever made it, the network/volume/... boxes to its left
    // would need to shift, which only a full render can do.
    double nx, ny, nw, nh;
    return clock_last_rect(output, &nx, &ny, &nw, &nh) && nw == w;
}
