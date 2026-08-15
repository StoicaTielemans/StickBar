#include "modules/powermenu/power_menu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/config.h"
#include "config/glyphs.h"
#include "hyprland/dispatch.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// Deliberately smaller than the reference PowerButton.qml's own 180x180 --
// matches this project's established "everywhere else runs noticeably more
// compact than the reference's Qt-driven sizing" precedent (see
// modules/media/media_popup.c's own thumbnail scale-down for the same
// reasoning), even though a full-screen overlay has plenty of room to spare.
#define BUTTON_SIZE 150.0
#define BUTTON_SPACING 24.0 // matches PowerMenu.qml's own RowLayout spacing exactly
#define ICON_LABEL_GAP 8.0  // matches PowerButton.qml's own ColumnLayout spacing exactly

typedef struct {
    double x, y, w, h;
} ButtonRect;

// Fixed 0=Logout, 1=Restart, 2=Shutdown order, matching PowerMenu.qml's own
// RowLayout order -- explicit per-button code below rather than a
// data-driven table, since a static table can't hold COLORS.* values
// (COLORS is an `extern const` object, not a compile-time constant, so it
// can't appear in a static initializer) and three buttons is small enough
// that explicit code reads at least as clearly as a table would anyway,
// matching this project's existing preference for a flat list over
// indirection at this kind of N (see wayland/pointer.c's own click-routing
// list).
typedef struct {
    Output *output;
    PopupSurface *surface;
    struct wl_surface *wl_surface;
    ButtonRect buttons[3];
} OutputOverlay;

static OutputOverlay g_overlays[MAX_TRACKED_OUTPUTS];
static int g_overlay_count = 0;
static bool g_open = false;
static AppContext *g_app = NULL;

// Which button (if any) the pointer currently sits over, and on which
// output's overlay -- only one can ever be hovered at a time (a single
// pointer), so this is process-wide state rather than per-overlay, same
// "one hovered_index int" shape modules/media/media_popup.c already uses
// for its own single-selection hover tracking.
static OutputOverlay *g_hovered_overlay = NULL;
static int g_hovered_button = -1;

void power_menu_init(AppContext *app) {
    g_app = app;
}

static void compute_buttons(Output *output, ButtonRect out[3]) {
    double total_w = 3.0 * BUTTON_SIZE + 2.0 * BUTTON_SPACING;
    double start_x = ((double)output->width - total_w) / 2.0;
    double y = ((double)output->height - BUTTON_SIZE) / 2.0;
    for (int i = 0; i < 3; i++) {
        out[i].x = start_x + (double)i * (BUTTON_SIZE + BUTTON_SPACING);
        out[i].y = y;
        out[i].w = BUTTON_SIZE;
        out[i].h = BUTTON_SIZE;
    }
}

static void action_logout(void) {
    hyprland_dispatch_exit();
}

static void action_restart(void) {
    if (system("systemctl reboot") != 0) {
        fprintf(stderr, "gridBar: failed to launch 'systemctl reboot'\n");
    }
}

static void action_shutdown(void) {
    if (system("systemctl poweroff") != 0) {
        fprintf(stderr, "gridBar: failed to launch 'systemctl poweroff'\n");
    }
}

static void draw_button(cairo_t *cr, ButtonRect *rect, bool hovered, Color accent, uint32_t glyph_cp,
                         const char *label) {
    // Matches PowerButton.qml's own `color: hovered ? surface0 : mantle` +
    // `border.color: accentColor` -- square corners (no radius, unlike this
    // project's other bordered boxes, which is already the default here
    // since module_box_draw_rect never rounds corners).
    Color fill = hovered ? COLORS.surface0 : COLORS.mantle;
    module_box_draw_rect(cr, rect->x, rect->y, rect->w, rect->h, fill, accent, (double)BAR.module_border_width);

    char glyph_buf[5];
    glyph_utf8(glyph_cp, glyph_buf);

    double icon_w, icon_h;
    module_box_measure_text(glyph_buf, FONT.display, false, &icon_w, &icon_h);
    double label_w, label_h;
    module_box_measure_text(label, FONT.heading, true, &label_w, &label_h);
    (void)icon_w;
    (void)label_w; // module_box_draw_text_centered() re-centers horizontally on its own

    // Icon-over-label stack, centered as a group within the button -- mirrors
    // PowerButton.qml's own centered ColumnLayout.
    double stack_h = icon_h + ICON_LABEL_GAP + label_h;
    double stack_top = rect->y + (rect->h - stack_h) / 2.0;

    module_box_draw_text_centered(cr, rect->x, stack_top, rect->w, icon_h, glyph_buf, FONT.display, false, accent);
    module_box_draw_text_centered(cr, rect->x, stack_top + icon_h + ICON_LABEL_GAP, rect->w, label_h, label,
                                   FONT.heading, true, accent);
}

static void draw_overlay(cairo_t *cr, void *user_data) {
    OutputOverlay *ov = user_data;

    // 30% alpha over COLORS.crust -- matches the reference's own
    // `color: "#4D" + Config.colors.crust.substring(1)` (0x4D / 255 ≈ 0.302).
    // CAIRO_OPERATOR_SOURCE, not the default OVER, is required here: this
    // surface is backed by one of two ping-ponged shm buffers (see
    // wayland/shm_buffer.c), so whichever buffer this redraw lands in still
    // holds *its own* previous frame's content, not a blank slate. Every
    // popup elsewhere in this project either paints something fully opaque
    // (which safely overwrites stale content regardless of operator) or is
    // literally transparent (draw_transparent(), which already uses SOURCE
    // for the same reason) -- this dim layer was the first translucent
    // full-surface paint in the codebase, and painting it with OVER was a
    // real bug caught live: every hover-triggered redraw_all() call (see
    // below) blended another 30%-alpha layer on top of whatever that same
    // buffer already had, so the background visibly got darker and darker
    // the more the pointer moved across the buttons, compounding toward
    // fully opaque instead of staying at a constant 30%.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, COLORS.crust.r, COLORS.crust.g, COLORS.crust.b, 0.302);
    cairo_paint(cr);
    cairo_restore(cr);

    compute_buttons(ov->output, ov->buttons);

    bool this_hovered = ov == g_hovered_overlay;
    draw_button(cr, &ov->buttons[0], this_hovered && g_hovered_button == 0, COLORS.yellow, GLYPH_ICON_LOGOUT,
                "Logout");
    draw_button(cr, &ov->buttons[1], this_hovered && g_hovered_button == 1, COLORS.sky, GLYPH_ICON_RESTART,
                "Restart");
    draw_button(cr, &ov->buttons[2], this_hovered && g_hovered_button == 2, COLORS.red, GLYPH_ICON_SHUTDOWN,
                "Shutdown");
}

static void redraw_all(void) {
    for (int i = 0; i < g_overlay_count; i++) {
        if (g_overlays[i].surface) {
            popup_surface_render_now(g_overlays[i].surface, draw_overlay, &g_overlays[i]);
        }
    }
}

static void close_overlay(void) {
    for (int i = 0; i < g_overlay_count; i++) {
        if (g_overlays[i].surface) {
            popup_surface_destroy(g_overlays[i].surface);
        }
    }
    memset(g_overlays, 0, sizeof(g_overlays));
    g_overlay_count = 0;
    g_open = false;
    g_hovered_overlay = NULL;
    g_hovered_button = -1;
}

static void open_overlay(void) {
    if (!g_app) {
        return; // power_menu_init() was never called -- shouldn't happen
    }

    g_overlay_count = 0;
    for (Output *o = g_app->outputs; o && g_overlay_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        OutputOverlay *ov = &g_overlays[g_overlay_count];
        // Full-screen, all four edges anchored with no margin, LAYER_OVERLAY
        // so it sits above the bar itself and every other popup -- same
        // "0,0 width/height + full anchor, let the compositor report the
        // real size via configure" shape every other popup's click-catcher
        // surfaces already use, except this one actually draws visible
        // content instead of staying transparent.
        ov->surface = popup_surface_create(
            g_app, o, 0, 0, 0, 0, 0, 0,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
        if (!ov->surface) {
            continue; // leave this slot for the next output to retry into
        }
        ov->output = o;
        ov->wl_surface = popup_surface_wl_surface(ov->surface);
        compute_buttons(o, ov->buttons);
        popup_surface_render_now(ov->surface, draw_overlay, ov);
        g_overlay_count++;
    }

    g_open = true;
    g_hovered_overlay = NULL;
    g_hovered_button = -1;
}

void power_menu_toggle(void) {
    if (g_open) {
        close_overlay();
    } else {
        open_overlay();
    }
}

bool power_menu_is_menu_surface(struct wl_surface *surface) {
    if (!g_open) {
        return false;
    }
    for (int i = 0; i < g_overlay_count; i++) {
        if (g_overlays[i].wl_surface == surface) {
            return true;
        }
    }
    return false;
}

static OutputOverlay *find_overlay(struct wl_surface *surface) {
    for (int i = 0; i < g_overlay_count; i++) {
        if (g_overlays[i].wl_surface == surface) {
            return &g_overlays[i];
        }
    }
    return NULL;
}

static int button_at(OutputOverlay *ov, double x, double y) {
    for (int i = 0; i < 3; i++) {
        ButtonRect *r = &ov->buttons[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            return i;
        }
    }
    return -1;
}

void power_menu_handle_motion(struct wl_surface *surface, double x, double y) {
    if (!g_open) {
        return;
    }
    OutputOverlay *ov = find_overlay(surface);
    if (!ov) {
        return;
    }
    int btn = button_at(ov, x, y);
    if (ov != g_hovered_overlay || btn != g_hovered_button) {
        g_hovered_overlay = ov;
        g_hovered_button = btn;
        // Redraws every output's overlay, not just this one -- simplest
        // correct way to also un-highlight whichever button/output was
        // previously hovered, and cheap enough given this only ever runs
        // while the rarely-open power menu is actually showing.
        redraw_all();
    }
}

void power_menu_handle_click(struct wl_surface *surface, double x, double y) {
    if (!g_open) {
        return;
    }
    OutputOverlay *ov = find_overlay(surface);
    if (!ov) {
        return;
    }

    int btn = button_at(ov, x, y);
    // Matches the reference's own unconditional `PowerMenuState.close()`
    // before dispatching an action -- and also covers a click that landed on
    // the dimmed background (btn == -1), matching the reference's own
    // full-surface `onClicked: PowerMenuState.close()`.
    close_overlay();

    switch (btn) {
        case 0: action_logout(); break;
        case 1: action_restart(); break;
        case 2: action_shutdown(); break;
        default: break; // background click -- just closes
    }
}
