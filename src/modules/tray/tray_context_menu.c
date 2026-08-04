#include "modules/tray/tray_context_menu.h"

#include <stdio.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/config.h"
#include "dbus/dbusmenu.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by modules/workspaces.c and modules/tray/tray.c.
#define MAX_TRACKED_OUTPUTS 8

// Vertical/horizontal geometry constants, chosen to visually mirror the
// reference TrayContextMenu.qml (values annotated with their QML counterpart)
// even where the underlying layout mechanism differs (plain arithmetic here vs.
// Qt's Column/anchors bindings there).
#define ROW_PADDING_V 16.0    // matches `height: labelText.implicitHeight + 16`
#define ROW_PADDING_H 24.0    // matches `labelText.implicitWidth + 24`
#define ROW_SPACING 6.0       // matches the Column's `spacing: 6`
#define OUTER_MARGIN 8.0      // matches `Config.bar.popupPadding`
#define LEFT_TEXT_MARGIN 12.0 // matches `leftMargin: 12`
// A separator contributes blank vertical spacing only, not a drawn rule line --
// matches the reference's own `visible: !entry.isSeparator` (invisible-gap, not
// chrome) exactly, per this project's stated goal of parity with that specific
// reference rather than an unrequested visual improvement.
#define SEPARATOR_HEIGHT 8.0

typedef struct {
    double y, h; // x is always OUTER_MARGIN, width is always g_menu.content_width
} RowLayout;

typedef struct {
    bool open;
    Output *output; // which output the popup itself is displayed on
    SniItem *item;  // which tray item this menu belongs to
    PopupSurface *popup;

    DbusMenuEntry entries[MAX_MENU_ENTRIES];
    RowLayout rows[MAX_MENU_ENTRIES];
    int entry_count;
    int hovered_index; // -1 if none

    double content_width, popup_width, popup_height;

    // One full-screen click-catcher per currently-known output, so an outside
    // click on *any* monitor closes the menu -- the reference's own dismiss
    // mechanism (HyprlandFocusGrab) captures compositor-wide, and this machine
    // has 2 monitors; a single-output catcher would leave the menu stuck open
    // if the user clicked the other one, a real regression versus the reference.
    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} MenuState;

static MenuState g_menu = { .hovered_index = -1 };

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    // CAIRO_OPERATOR_SOURCE forces the destination to exactly this fully
    // transparent color rather than blending -- not strictly necessary for a
    // freshly-allocated (typically zeroed) shm buffer, but makes the intent
    // explicit rather than relying on that incidental starting state.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_menu(cairo_t *cr, void *user_data) {
    (void)user_data;

    module_box_draw_rect(cr, 0, 0, g_menu.popup_width, g_menu.popup_height,
                          COLORS.mantle, COLORS.overlay0, (double)BAR.module_border_width);

    for (int i = 0; i < g_menu.entry_count; i++) {
        DbusMenuEntry *entry = &g_menu.entries[i];
        if (entry->is_separator) {
            continue;
        }
        RowLayout *row = &g_menu.rows[i];

        if (i == g_menu.hovered_index) {
            module_box_draw_rect(cr, OUTER_MARGIN, row->y, g_menu.content_width, row->h,
                                  COLORS.surface1, (Color){ 0, 0, 0, 0 }, 0.0);
        }

        Color text_color = entry->enabled ? COLORS.text : COLORS.overlay0;
        module_box_draw_text_left(cr, OUTER_MARGIN + LEFT_TEXT_MARGIN, row->y, row->h,
                                   entry->label, FONT.label, false, text_color);
    }
}

static void redraw(void) {
    if (g_menu.popup) {
        popup_surface_render_now(g_menu.popup, draw_menu, NULL);
    }
}

// Computes content_width/popup_width/popup_height and each row's (y, h) from
// the already-fetched g_menu.entries -- must run before popup_surface_create(),
// since (unlike the bar) a popup has no "ask the compositor for a width" option
// and needs an explicit size up front.
static void compute_layout(void) {
    double max_label_w = 0.0;
    double row_heights[MAX_MENU_ENTRIES];

    for (int i = 0; i < g_menu.entry_count; i++) {
        if (g_menu.entries[i].is_separator) {
            row_heights[i] = SEPARATOR_HEIGHT;
            continue;
        }
        double w, h;
        module_box_measure_text(g_menu.entries[i].label, FONT.label, false, &w, &h);
        if (w > max_label_w) {
            max_label_w = w;
        }
        row_heights[i] = h + ROW_PADDING_V;
    }

    // Every row shares the same width (the widest label's), matching the
    // reference's own Math.max(menuColumn.implicitWidth, ...) resolving to a
    // uniform row width across all entries.
    g_menu.content_width = max_label_w + ROW_PADDING_H;
    g_menu.popup_width = g_menu.content_width + OUTER_MARGIN * 2.0;

    double y = OUTER_MARGIN;
    for (int i = 0; i < g_menu.entry_count; i++) {
        g_menu.rows[i].y = y;
        g_menu.rows[i].h = row_heights[i];
        y += row_heights[i];
        if (i < g_menu.entry_count - 1) {
            y += ROW_SPACING;
        }
    }
    g_menu.popup_height = y + OUTER_MARGIN;
}

static void create_catchers(AppContext *app) {
    g_menu.catcher_count = 0;
    for (Output *o = app->outputs; o && g_menu.catcher_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create(
            app, o, 0, 0, 0, 0,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP);
        if (!catcher) {
            continue;
        }
        popup_surface_render_now(catcher, draw_transparent, NULL);
        g_menu.catchers[g_menu.catcher_count] = catcher;
        g_menu.catcher_surfaces[g_menu.catcher_count] = popup_surface_wl_surface(catcher);
        g_menu.catcher_count++;
    }
}

static void destroy_catchers(void) {
    for (int i = 0; i < g_menu.catcher_count; i++) {
        popup_surface_destroy(g_menu.catchers[i]);
        g_menu.catchers[i] = NULL;
        g_menu.catcher_surfaces[i] = NULL;
    }
    g_menu.catcher_count = 0;
}

void tray_context_menu_open(Output *output, SniItem *item, double icon_x, double icon_y, double icon_w, double icon_h) {
    tray_context_menu_close(); // at most one menu open at a time

    sd_bus *bus = sni_item_bus(item);
    int count = 0;
    if (!dbusmenu_fetch(bus, sni_item_bus_name(item), sni_item_menu_path(item), g_menu.entries, &count) || count == 0) {
        return;
    }

    g_menu.entry_count = count;
    g_menu.hovered_index = -1;
    g_menu.item = item;
    g_menu.output = output;

    compute_layout();

    // Reference semantics (anchor: Bottom|Right, gravity: Bottom|Left -- the
    // popup's top-right corner attaches to the icon's bottom-right corner and
    // it grows down-and-left from there) translate to: anchor TOP|RIGHT with
    // top_margin = the icon's bottom edge, right_margin = the distance from the
    // icon's right edge to the output's own right edge.
    // Subtracting BAR.height compensates for Hyprland appearing to measure a
    // layer surface's own top_margin from the edge of its *already-reserved*
    // exclusive-zone region rather than the raw output edge the spec text
    // describes, when the margin is set by the same client that owns that
    // reservation (the bar's own layer surface). Reasoned from spec text plus
    // the observed symptom (a suspiciously round one-bar-height offset without
    // this subtraction) -- verify by screenshot before trusting this blindly.
    int top_margin = (int)(icon_y + icon_h) - BAR.height;
    int right_margin = output->width - (int)(icon_x + icon_w);
    if (right_margin < 0) {
        // Known limitation (also true of the reference): no reflow if the menu
        // would overflow past the output's left edge -- clamp rather than
        // produce a negative margin, which would push the popup off-screen
        // instead of just looking slightly misaligned.
        right_margin = 0;
    }

    g_menu.popup = popup_surface_create(output->app, output, (int)g_menu.popup_width, (int)g_menu.popup_height,
                                         top_margin, right_margin,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                                         ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    if (!g_menu.popup) {
        return;
    }

    create_catchers(output->app);
    g_menu.open = true;
    redraw(); // no-op until the compositor's configure arrives; see popup_surface.c
}

void tray_context_menu_close(void) {
    if (!g_menu.open) {
        return;
    }
    popup_surface_destroy(g_menu.popup);
    g_menu.popup = NULL;
    destroy_catchers();
    g_menu.open = false;
    g_menu.item = NULL;
    g_menu.output = NULL;
    g_menu.entry_count = 0;
    g_menu.hovered_index = -1;
}

bool tray_context_menu_is_menu_surface(struct wl_surface *surface) {
    return g_menu.open && g_menu.popup && popup_surface_wl_surface(g_menu.popup) == surface;
}

bool tray_context_menu_is_catcher_surface(struct wl_surface *surface) {
    if (!g_menu.open) {
        return false;
    }
    for (int i = 0; i < g_menu.catcher_count; i++) {
        if (g_menu.catcher_surfaces[i] == surface) {
            return true;
        }
    }
    return false;
}

// Finds which non-separator row (x, y) lands on, or -1 if it lands on the
// border, a separator's blank gap, or outside every row entirely.
static int row_at(double x, double y) {
    if (x < OUTER_MARGIN || x >= OUTER_MARGIN + g_menu.content_width) {
        return -1;
    }
    for (int i = 0; i < g_menu.entry_count; i++) {
        if (g_menu.entries[i].is_separator) {
            continue;
        }
        RowLayout *row = &g_menu.rows[i];
        if (y >= row->y && y < row->y + row->h) {
            return i;
        }
    }
    return -1;
}

void tray_context_menu_handle_motion(double x, double y) {
    if (!g_menu.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx != g_menu.hovered_index) {
        g_menu.hovered_index = idx;
        redraw();
    }
}

void tray_context_menu_handle_click(double x, double y) {
    if (!g_menu.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx < 0) {
        return;
    }
    DbusMenuEntry *entry = &g_menu.entries[idx];
    if (!entry->enabled) {
        // Matches the reference's click gate (`if (entry.enabled) { ... }`) --
        // a disabled entry is still hover-highlightable (row_at() doesn't
        // exclude it) but never triggers or closes the menu.
        return;
    }

    dbusmenu_trigger(sni_item_bus(g_menu.item), sni_item_bus_name(g_menu.item), sni_item_menu_path(g_menu.item), entry->id);
    tray_context_menu_close();
}
