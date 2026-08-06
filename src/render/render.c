#include "render/render.h"

#include "config/config.h"
#include "modules/clock/clock.h"
#include "modules/cpu/cpu.h"
#include "modules/gpu/gpu.h"
#include "modules/media/media.h"
#include "modules/mem/mem.h"
#include "modules/mic/mic.h"
#include "modules/tray/tray.h"
#include "modules/volume/volume.h"
#include "modules/workspaces/workspaces.h"

void render_output(cairo_t *cr, Output *output, int width, int height) {
    // Background fill. This also implicitly erases everything from the previous
    // frame (e.g. a workspace pill that's since stopped being occupied), so no
    // separate clear/memset step is needed anywhere else in the render path.
    cairo_set_source_rgba(cr, COLORS.mantle.r, COLORS.mantle.g, COLORS.mantle.b, COLORS.mantle.a);
    cairo_paint(cr);

    // Left region: workspace pills flush against the left edge, then CPU/GPU/
    // memory -- matches the reference bar's actual leftLayout order
    // (ArchLogoWidget, HyprlandWindows, SystemMonitorWidget; ArchLogoWidget
    // isn't built here, and SystemMonitorWidget's combined CPU/GPU/MEM stats
    // are split into three separate modules per explicit user request, kept
    // in that same relative left-to-right order).
    double workspaces_right_x = workspaces_render(cr, output, 0.0, 0.0, (double)height);
    double cpu_right_x = cpu_render(cr, output, workspaces_right_x, 0.0, (double)height);
    double gpu_right_x = gpu_render(cr, output, cpu_right_x, 0.0, (double)height);
    mem_render(cr, output, gpu_right_x, 0.0, (double)height);

    // Right region: the reference Bar.qml's actual rightLayout order is Mic,
    // Volume, Network, Clock, Tray -- Tray is rightmost, drawn first here since
    // this loop works right-to-left. Network isn't built yet, so Volume sits
    // directly left of Clock for now.
    double tray_left_x = tray_render(cr, output, (double)width, 0.0, (double)height);
    double clock_left_x = clock_render(cr, output, tray_left_x, 0.0, (double)height);
    double volume_left_x = volume_render(cr, output, clock_left_x, 0.0, (double)height);
    mic_render(cr, output, volume_left_x, 0.0, (double)height);

    // Center region: matches the reference bar's own centerLayout, which
    // holds only the media widget, horizontally centered
    // (`anchors.horizontalCenter: parent.horizontalCenter`) rather than
    // anchored to either edge like every other module here.
    media_render(cr, output, (double)width, 0.0, (double)height);
}
