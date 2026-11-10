#include "modules/powermenu/power_menu.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bar/popup_surface.h"
#include "config/config.h"
#include "config/glyphs.h"
#include "hyprland/dispatch.h"
#include "render/module_box.h"
#include "testing/test_control.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Header,
// pill and card styling match the dropdowns on purpose: a fixed-size panel
// centered on each output's dimmed background, three action cards in a row.
#define CARD_W 116.0
#define CARD_H 112.0
#define CARD_GAP 8.0
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define COL_GAP 10.0
#define ICON_LABEL_GAP 4.0
#define LABEL_DETAIL_GAP 2.0
#define CONTENT_W (3.0 * CARD_W + 2.0 * CARD_GAP)
#define PANEL_W (CONTENT_W + OUTER_MARGIN * 2.0)
#define PANEL_H (OUTER_MARGIN + HEADER_H + SECTION_GAP + CARD_H + OUTER_MARGIN)

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
    ButtonRect panel;
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

// Header text, read once at open: who's about to be logged out, and for how long
// the machine has been up (empty if /proc/uptime couldn't be read: no pill).
static char g_identity[128];
static char g_uptime[32];

// What the last card click would have run, on a test-control instance -- see
// power_menu_handle_click().
static const char *g_test_last_action = "";

void power_menu_init(AppContext *app) {
    g_app = app;
}

static void compute_layout(OutputOverlay *ov) {
    // The surface's real size, not the output's: anchored to all four edges,
    // it still stops at the bars' exclusive zones, so it's shorter than the
    // output (and centering by output height would land low). The output's
    // size is only the stand-in for before the first configure.
    int surface_w = 0, surface_h = 0;
    if (ov->surface) {
        popup_surface_size(ov->surface, &surface_w, &surface_h);
    }
    if (surface_w <= 0 || surface_h <= 0) {
        surface_w = ov->output->width;
        surface_h = ov->output->height;
    }
    ov->panel = (ButtonRect){floor((surface_w - PANEL_W) / 2.0), floor((surface_h - PANEL_H) / 2.0), PANEL_W, PANEL_H};
    for (int i = 0; i < 3; i++) {
        ov->buttons[i].x = ov->panel.x + OUTER_MARGIN + (double)i * (CARD_W + CARD_GAP);
        ov->buttons[i].y = ov->panel.y + OUTER_MARGIN + HEADER_H + SECTION_GAP;
        ov->buttons[i].w = CARD_W;
        ov->buttons[i].h = CARD_H;
    }
}

static void read_header_info(void) {
    char host[64] = "";
    gethostname(host, sizeof(host) - 1);
    const char *user = getenv("USER");
    snprintf(g_identity, sizeof(g_identity), "%s@%s", user ? user : "?", host);

    g_uptime[0] = '\0';
    FILE *f = fopen("/proc/uptime", "r");
    double seconds = 0.0;
    if (f) {
        if (fscanf(f, "%lf", &seconds) == 1) {
            long minutes = (long)(seconds / 60.0);
            long hours = minutes / 60, days = hours / 24;
            if (days > 0) {
                snprintf(g_uptime, sizeof(g_uptime), "up %ldd %ldh", days, hours % 24);
            } else if (hours > 0) {
                snprintf(g_uptime, sizeof(g_uptime), "up %ldh %ldm", hours, minutes % 60);
            } else {
                snprintf(g_uptime, sizeof(g_uptime), "up %ldm", minutes);
            }
        }
        fclose(f);
    }
}

static void action_logout(void) {
    hyprland_dispatch_exit();
}

static void action_restart(void) {
    if (system("systemctl reboot") != 0) {
        fprintf(stderr, "stickBar: failed to launch 'systemctl reboot'\n");
    }
}

static void action_shutdown(void) {
    if (system("systemctl poweroff") != 0) {
        fprintf(stderr, "stickBar: failed to launch 'systemctl poweroff'\n");
    }
}

// Same language as the dropdowns' cards: `base` with a grey border, lightening
// on hover -- plus the action's own colour on the border, so it's clear which
// one a click would run.
static void draw_card(cairo_t *cr, ButtonRect *rect, bool hovered, Color accent, uint32_t glyph_cp, const char *label,
                      const char *detail) {
    module_box_draw_rect(cr, rect->x, rect->y, rect->w, rect->h, hovered ? COLORS.surface0 : COLORS.base,
                         hovered ? accent : COLORS.surface1, hovered ? 2.0 : 1.0);

    char glyph_buf[5];
    glyph_utf8(glyph_cp, glyph_buf);

    double unused, icon_h, label_h, detail_h;
    module_box_measure_text(glyph_buf, FONT.display, false, &unused, &icon_h);
    module_box_measure_text(label, FONT.label, true, &unused, &label_h);
    module_box_measure_text(detail, FONT.xs, false, &unused, &detail_h);

    // Icon-over-label-over-detail stack, centered as a group within the card.
    double y = rect->y + (rect->h - (icon_h + ICON_LABEL_GAP + label_h + LABEL_DETAIL_GAP + detail_h)) / 2.0;
    module_box_draw_text_centered(cr, rect->x, y, rect->w, icon_h, glyph_buf, FONT.display, false, accent);
    y += icon_h + ICON_LABEL_GAP;
    module_box_draw_text_centered(cr, rect->x, y, rect->w, label_h, label, FONT.label, true,
                                  hovered ? accent : COLORS.text);
    y += label_h + LABEL_DETAIL_GAP;
    module_box_draw_text_centered(cr, rect->x, y, rect->w, detail_h, detail, FONT.xs, false, COLORS.overlay0);
}

static void draw_header(cairo_t *cr, ButtonRect *panel) {
    double x = panel->x + OUTER_MARGIN, y = panel->y + OUTER_MARGIN;
    double text_right = x + CONTENT_W;

    if (g_uptime[0]) {
        double pill_w = module_box_text_width(g_uptime, FONT.xs, true) + PILL_PAD_X * 2.0;
        double pill_x = x + CONTENT_W - pill_w;
        double pill_y = y + (HEADER_H - PILL_H) / 2.0;
        module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, COLORS.sapphire, COLORS.mantle, 0.0);
        module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, g_uptime, FONT.xs, true, COLORS.mantle);
        text_right = pill_x - COL_GAP;
    }

    // Clipped short of the pill: a long hostname must not draw over it.
    cairo_save(cr);
    cairo_rectangle(cr, x, y, text_right - x, HEADER_H);
    cairo_clip(cr);
    char icon[5], summary[sizeof(g_identity) + 8];
    glyph_utf8(GLYPH_ICON_SHUTDOWN, icon);
    snprintf(summary, sizeof(summary), "  ·  %s", g_identity);
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, COLORS.sapphire);
    double tx = x + module_box_text_width(icon, FONT.heading, false) + 8.0;
    module_box_draw_text_left(cr, tx, y, HEADER_H, "Power", FONT.md, true, COLORS.text);
    tx += module_box_text_width("Power", FONT.md, true);
    module_box_draw_text_left(cr, tx, y, HEADER_H, summary, FONT.sm, false, COLORS.subtext0);
    cairo_restore(cr);
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

    compute_layout(ov);

    // Sapphire, the Arch logo box's colour: the module's accent on the
    // border, like every other popup.
    module_box_draw_rect(cr, ov->panel.x, ov->panel.y, ov->panel.w, ov->panel.h, COLORS.mantle, COLORS.sapphire,
                         (double)BAR.module_border_width);
    draw_header(cr, &ov->panel);

    bool this_hovered = ov == g_hovered_overlay;
    draw_card(cr, &ov->buttons[0], this_hovered && g_hovered_button == 0, COLORS.yellow, GLYPH_ICON_LOGOUT, "Logout",
              "End this session");
    draw_card(cr, &ov->buttons[1], this_hovered && g_hovered_button == 1, COLORS.sky, GLYPH_ICON_RESTART, "Restart",
              "Reboot the machine");
    draw_card(cr, &ov->buttons[2], this_hovered && g_hovered_button == 2, COLORS.red, GLYPH_ICON_SHUTDOWN, "Shutdown",
              "Power off");
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

    read_header_info();

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
        compute_layout(ov);
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
    // Inside the panel but not on a card (its header, the gaps) is absorbed,
    // the same as a click on any dropdown's own surface.
    ButtonRect *p = &ov->panel;
    if (btn < 0 && x >= p->x && x < p->x + p->w && y >= p->y && y < p->y + p->h) {
        return;
    }

    // Matches the reference's own unconditional `PowerMenuState.close()`
    // before dispatching an action -- and also covers a click that landed on
    // the dimmed background (btn == -1), matching the reference's own
    // full-surface `onClicked: PowerMenuState.close()`.
    close_overlay();

    // A test instance must never be able to end the session or the machine:
    // it records what would have run instead (tests/test_power_menu.py).
    if (test_control_enabled()) {
        g_test_last_action = btn == 0 ? "logout" : btn == 1 ? "restart" : btn == 2 ? "shutdown" : "";
        return;
    }

    switch (btn) {
        case 0: action_logout(); break;
        case 1: action_restart(); break;
        case 2: action_shutdown(); break;
        default: break; // background click -- just closes
    }
}

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

const char *power_menu_test_last_action(void) { return g_test_last_action; }

bool power_menu_test_target(const char *part, double *x, double *y) {
    if (!g_open || g_overlay_count == 0) {
        return false;
    }
    OutputOverlay *ov = &g_overlays[0];
    int card = strcmp(part, "logout") == 0 ? 0 : strcmp(part, "restart") == 0 ? 1 : strcmp(part, "shutdown") == 0 ? 2 : -1;
    if (card >= 0) {
        *x = ov->buttons[card].x + CARD_W / 2.0;
        *y = ov->buttons[card].y + CARD_H / 2.0;
    } else if (strcmp(part, "corner") == 0) {
        *x = ov->panel.x;
        *y = ov->panel.y;
    } else if (strcmp(part, "header") == 0) {
        *x = ov->panel.x + PANEL_W / 2.0;
        *y = ov->panel.y + OUTER_MARGIN + HEADER_H / 2.0;
    } else {
        return false;
    }
    return true;
}
