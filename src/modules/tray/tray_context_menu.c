#include "modules/tray/tray_context_menu.h"

#include <stdio.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/config.h"
#include "config/glyphs.h"
#include "dbus/dbusmenu.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by modules/workspaces.c and modules/tray/tray.c.
#define MAX_TRACKED_OUTPUTS 8

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Same
// header/card language as the other dropdowns, kept neutral grey (overlay0
// border, per explicit user request): the tray is the one module without an
// accent colour of its own.
#define OUTER_MARGIN 12.0
#define HEADER_H 22.0
#define HEADER_ICON 20.0 // tray icons are pre-scaled to BAR.height (20px)
#define SECTION_GAP 10.0
#define CARD_GAP 6.0
#define CARD_PAD 3.0
#define ROW_H 26.0
#define ROW_PAD_X 10.0
#define HOVER_BAR_W 3.0
#define COL_GAP 8.0
#define MIN_CONTENT_W 200.0

typedef struct {
    double y, h; // row rect, relative to the popup (x/w are the card's inner area)
} RowLayout;

typedef struct {
    double y, h;
} CardLayout;

typedef struct {
    bool open;
    Output *output; // which output the popup itself is displayed on
    SniItem *item;  // which tray item this menu belongs to
    PopupSurface *popup;

    DbusMenuEntry entries[MAX_MENU_ENTRIES];
    RowLayout rows[MAX_MENU_ENTRIES]; // meaningful only for non-separator entries
    int entry_count;
    int hovered_index; // -1 if none

    // Separators split the entries into groups, each drawn as its own card
    // (a separator is the gap between two cards, not a drawn line).
    CardLayout cards[MAX_MENU_ENTRIES];
    int card_count;
    int item_count;     // non-separator entries, for the header
    bool any_toggle;    // reserve the toggle column on every row if any entry has one
    char title[128];    // the item's name, snapshotted at open

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

static double text_width(const char *text, int size, bool bold) {
    double w, h;
    module_box_measure_text(text, size, bold, &w, &h);
    (void)h;
    return w;
}

static double toggle_col_w(void) {
    char buf[5];
    glyph_utf8(GLYPH_ICON_CHECKBOX_ON, buf);
    return text_width(buf, FONT.label, false) + COL_GAP;
}

static double chevron_w(void) {
    char buf[5];
    glyph_utf8(GLYPH_CHEVRON_RIGHT, buf);
    return text_width(buf, FONT.xs, false);
}

static double card_inner_x(void) { return OUTER_MARGIN + CARD_PAD; }
static double card_inner_w(void) { return g_menu.content_width - CARD_PAD * 2.0; }

static void header_summary(char *out, size_t out_size) {
    snprintf(out, out_size, "  ·  %d item%s", g_menu.item_count, g_menu.item_count == 1 ? "" : "s");
}

static void draw_row(cairo_t *cr, int i) {
    DbusMenuEntry *entry = &g_menu.entries[i];
    RowLayout *row = &g_menu.rows[i];
    double x = card_inner_x(), w = card_inner_w();
    // Disabled entries are dimmed and never highlight (they can't be clicked).
    if (i == g_menu.hovered_index && entry->enabled) {
        module_box_draw_rect(cr, x, row->y, w, row->h, COLORS.surface0, COLORS.mantle, 0.0);
        module_box_draw_rect(cr, x, row->y, HOVER_BAR_W, row->h, COLORS.overlay2, COLORS.mantle, 0.0);
    }
    Color text_color = entry->enabled ? COLORS.text : COLORS.overlay0;
    double tx = x + ROW_PAD_X;
    if (g_menu.any_toggle) {
        if (entry->toggle != DBUSMENU_TOGGLE_NONE) {
            uint32_t glyph = entry->toggle == DBUSMENU_TOGGLE_RADIO
                                 ? (entry->toggled ? GLYPH_ICON_RADIO_ON : GLYPH_ICON_RADIO_OFF)
                                 : (entry->toggled ? GLYPH_ICON_CHECKBOX_ON : GLYPH_ICON_CHECKBOX_OFF);
            char buf[5];
            glyph_utf8(glyph, buf);
            module_box_draw_text_left(cr, tx, row->y, row->h, buf, FONT.label, false,
                                      entry->toggled && entry->enabled ? COLORS.text : COLORS.overlay0);
        }
        tx += toggle_col_w();
    }
    module_box_draw_text_left(cr, tx, row->y, row->h, entry->label, FONT.label, false, text_color);
    if (entry->has_submenu) {
        // Shown so the entry doesn't look like a plain action -- opening
        // submenus isn't supported yet (see docs/limitations.md).
        char buf[5];
        glyph_utf8(GLYPH_CHEVRON_RIGHT, buf);
        module_box_draw_text_left(cr, x + w - ROW_PAD_X - chevron_w(), row->y, row->h, buf, FONT.xs, false,
                                  COLORS.overlay1);
    }
}

static void draw_menu(cairo_t *cr, void *user_data) {
    (void)user_data;

    module_box_draw_rect(cr, 0, 0, g_menu.popup_width, g_menu.popup_height,
                          COLORS.mantle, COLORS.overlay0, (double)BAR.module_border_width);

    // Header: the item's own tray icon, its name, and the entry count.
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    cairo_surface_t *icon = sni_item_icon_surface(g_menu.item);
    if (icon) {
        cairo_save(cr);
        cairo_set_source_surface(cr, icon, x, y + (HEADER_H - HEADER_ICON) / 2.0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        module_box_draw_rect(cr, x, y + (HEADER_H - HEADER_ICON) / 2.0, HEADER_ICON, HEADER_ICON,
                             COLORS.overlay0, COLORS.mantle, 0.0);
    }
    double tx = x + HEADER_ICON + 8.0;
    module_box_draw_text_left(cr, tx, y, HEADER_H, g_menu.title, FONT.md, true, COLORS.text);
    tx += text_width(g_menu.title, FONT.md, true);
    char summary[48];
    header_summary(summary, sizeof(summary));
    module_box_draw_text_left(cr, tx, y, HEADER_H, summary, FONT.sm, false, COLORS.subtext0);

    for (int c = 0; c < g_menu.card_count; c++) {
        module_box_draw_rect(cr, OUTER_MARGIN, g_menu.cards[c].y, g_menu.content_width, g_menu.cards[c].h,
                             COLORS.base, COLORS.surface1, 1.0);
    }
    for (int i = 0; i < g_menu.entry_count; i++) {
        if (!g_menu.entries[i].is_separator) {
            draw_row(cr, i);
        }
    }
}

static void redraw(void) {
    if (g_menu.popup) {
        popup_surface_render_now(g_menu.popup, draw_menu, NULL);
    }
}

// Computes the cards, rows and popup size from the already-fetched
// g_menu.entries -- must run before popup_surface_create(), since (unlike the
// bar) a popup has no "ask the compositor for a width" option and needs an
// explicit size up front. Separators split the entries into cards; leading,
// trailing and repeated separators produce no empty card.
static void compute_layout(void) {
    g_menu.item_count = 0;
    g_menu.any_toggle = false;
    double max_row_w = 0.0;
    for (int i = 0; i < g_menu.entry_count; i++) {
        DbusMenuEntry *entry = &g_menu.entries[i];
        if (entry->is_separator) {
            continue;
        }
        g_menu.item_count++;
        g_menu.any_toggle |= entry->toggle != DBUSMENU_TOGGLE_NONE;
        double w = text_width(entry->label, FONT.label, false) + (entry->has_submenu ? COL_GAP + chevron_w() : 0.0);
        if (w > max_row_w) {
            max_row_w = w;
        }
    }
    double row_w = ROW_PAD_X * 2.0 + (g_menu.any_toggle ? toggle_col_w() : 0.0) + max_row_w + CARD_PAD * 2.0;
    char summary[48];
    header_summary(summary, sizeof(summary));
    double header_w = HEADER_ICON + 8.0 + text_width(g_menu.title, FONT.md, true) + text_width(summary, FONT.sm, false);
    g_menu.content_width = row_w > header_w ? row_w : header_w;
    if (g_menu.content_width < MIN_CONTENT_W) {
        g_menu.content_width = MIN_CONTENT_W;
    }
    g_menu.popup_width = g_menu.content_width + OUTER_MARGIN * 2.0;

    double y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    g_menu.card_count = 0;
    bool in_card = false;
    for (int i = 0; i < g_menu.entry_count; i++) {
        if (g_menu.entries[i].is_separator) {
            if (in_card) {
                CardLayout *card = &g_menu.cards[g_menu.card_count - 1];
                card->h = y + CARD_PAD - card->y;
                y = card->y + card->h + CARD_GAP;
                in_card = false;
            }
            continue;
        }
        if (!in_card) {
            g_menu.cards[g_menu.card_count].y = y;
            g_menu.card_count++;
            y += CARD_PAD;
            in_card = true;
        }
        g_menu.rows[i].y = y;
        g_menu.rows[i].h = ROW_H;
        y += ROW_H;
    }
    if (in_card) {
        CardLayout *card = &g_menu.cards[g_menu.card_count - 1];
        card->h = y + CARD_PAD - card->y;
        y = card->y + card->h;
    } else if (g_menu.card_count > 0) {
        y -= CARD_GAP; // a trailing separator left one gap too many
    }
    g_menu.popup_height = y + OUTER_MARGIN;
}

static void create_catchers(AppContext *app) {
    g_menu.catcher_count = 0;
    for (Output *o = app->outputs; o && g_menu.catcher_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create_catcher(app, o);
        if (!catcher) {
            continue;
        }
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
    snprintf(g_menu.title, sizeof(g_menu.title), "%s", sni_item_title(item)[0] ? sni_item_title(item) : "Menu");

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
                                         top_margin, right_margin, 0, 0,
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
// header, a gap between cards, or outside every row entirely.
static int row_at(double x, double y) {
    if (x < card_inner_x() || x >= card_inner_x() + card_inner_w()) {
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
        // a disabled entry never triggers or closes the menu (and isn't
        // highlighted on hover, see draw_row()).
        return;
    }

    dbusmenu_trigger(sni_item_bus(g_menu.item), sni_item_bus_name(g_menu.item), sni_item_menu_path(g_menu.item), entry->id);
    tray_context_menu_close();
}

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

int tray_context_menu_test_entry_count(void) { return g_menu.open ? g_menu.entry_count : -1; }

const DbusMenuEntry *tray_context_menu_test_entry(int index) {
    return (g_menu.open && index >= 0 && index < g_menu.entry_count) ? &g_menu.entries[index] : NULL;
}

bool tray_context_menu_test_target(const char *label, double *x, double *y) {
    for (int i = 0; g_menu.open && i < g_menu.entry_count; i++) {
        if (!g_menu.entries[i].is_separator && strcmp(g_menu.entries[i].label, label) == 0) {
            *x = card_inner_x() + card_inner_w() / 2.0;
            *y = g_menu.rows[i].y + g_menu.rows[i].h / 2.0;
            return true;
        }
    }
    return false;
}
