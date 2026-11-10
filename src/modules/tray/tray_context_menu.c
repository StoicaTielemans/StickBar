#include "modules/tray/tray_context_menu.h"

#include <stdio.h>
#include <string.h>

#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "config/config.h"
#include "config/glyphs.h"
#include "dbus/dbusmenu.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Same
// header/card language as the other dropdowns, kept neutral grey (overlay0
// border, per explicit user request): the tray is the one module without an
// accent colour of its own.
#define HEADER_ICON 20.0 // tray icons are pre-scaled to BAR.height (20px)
#define CARD_GAP 6.0  // tighter than POPUP_CARD_GAP (8): menus can run long, entries stay compact
#define CARD_PAD 3.0
#define ROW_H 26.0
#define ROW_PAD_X 10.0
#define HOVER_BAR_W 3.0
#define COL_GAP 8.0   // tighter than POPUP_COL_GAP (10), for the same reason
#define MIN_CONTENT_W 200.0

typedef struct {
    double y, h; // row rect, relative to the popup (x/w are the card's inner area)
} RowLayout;

typedef struct {
    double y, h;
} CardLayout;

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    SniItem *item;  // which tray item this menu belongs to

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
} MenuState;

static MenuState g_menu = { .hovered_index = -1 };

static double toggle_col_w(void) {
    char buf[5];
    glyph_utf8(GLYPH_ICON_CHECKBOX_ON, buf);
    return module_box_text_width(buf, FONT.label, false) + COL_GAP;
}

static double chevron_w(void) {
    char buf[5];
    glyph_utf8(GLYPH_CHEVRON_RIGHT, buf);
    return module_box_text_width(buf, FONT.xs, false);
}

static double card_inner_x(void) { return POPUP_OUTER_MARGIN + CARD_PAD; }
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

    popup_draw_frame(cr, g_menu.popup_width, g_menu.popup_height, COLORS.overlay0);

    // Header: the item's own tray icon, its name, and the entry count.
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    cairo_surface_t *icon = sni_item_icon_surface(g_menu.item);
    if (icon) {
        cairo_save(cr);
        cairo_set_source_surface(cr, icon, x, y + (POPUP_HEADER_H - HEADER_ICON) / 2.0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        module_box_draw_rect(cr, x, y + (POPUP_HEADER_H - HEADER_ICON) / 2.0, HEADER_ICON, HEADER_ICON,
                             COLORS.overlay0, COLORS.mantle, 0.0);
    }
    double tx = x + HEADER_ICON + POPUP_HEADER_ICON_GAP;
    module_box_draw_text_left(cr, tx, y, POPUP_HEADER_H, g_menu.title, FONT.md, true, COLORS.text);
    tx += module_box_text_width(g_menu.title, FONT.md, true);
    char summary[48];
    header_summary(summary, sizeof(summary));
    module_box_draw_text_left(cr, tx, y, POPUP_HEADER_H, summary, FONT.sm, false, COLORS.subtext0);

    for (int c = 0; c < g_menu.card_count; c++) {
        popup_draw_card(cr, POPUP_OUTER_MARGIN, g_menu.cards[c].y, g_menu.content_width, g_menu.cards[c].h, false,
                        false, COLORS.surface1);
    }
    for (int i = 0; i < g_menu.entry_count; i++) {
        if (!g_menu.entries[i].is_separator) {
            draw_row(cr, i);
        }
    }
}

static void redraw(void) {
    if (g_menu.host.popup) {
        popup_surface_render_now(g_menu.host.popup, draw_menu, NULL);
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
        double w = module_box_text_width(entry->label, FONT.label, false) +
                   (entry->has_submenu ? COL_GAP + chevron_w() : 0.0);
        if (w > max_row_w) {
            max_row_w = w;
        }
    }
    double row_w = ROW_PAD_X * 2.0 + (g_menu.any_toggle ? toggle_col_w() : 0.0) + max_row_w + CARD_PAD * 2.0;
    char summary[48];
    header_summary(summary, sizeof(summary));
    double header_w = HEADER_ICON + POPUP_HEADER_ICON_GAP + module_box_text_width(g_menu.title, FONT.md, true) +
                      module_box_text_width(summary, FONT.sm, false);
    g_menu.content_width = row_w > header_w ? row_w : header_w;
    if (g_menu.content_width < MIN_CONTENT_W) {
        g_menu.content_width = MIN_CONTENT_W;
    }
    g_menu.popup_width = g_menu.content_width + POPUP_OUTER_MARGIN * 2.0;

    double y = POPUP_OUTER_MARGIN + POPUP_HEADER_H + POPUP_SECTION_GAP;
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
    g_menu.popup_height = y + POPUP_OUTER_MARGIN;
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
    g_menu.host.output = output;
    snprintf(g_menu.title, sizeof(g_menu.title), "%s", sni_item_title(item)[0] ? sni_item_title(item) : "Menu");

    compute_layout();

    // Under the icon, opening leftward from its right edge (bar/popup_host.c,
    // which also has the BAR.height margin correction this menu was the
    // first to need). Known limitation, as in the reference: no reflow if
    // the menu would overflow the output's left edge.
    if (!popup_host_open(&g_menu.host, output, POPUP_ANCHOR_RIGHT, icon_x, icon_y, icon_w, icon_h,
                         (int)g_menu.popup_width, (int)g_menu.popup_height)) {
        return;
    }
    redraw(); // no-op until the compositor's configure arrives; see popup_surface.c
}

void tray_context_menu_close(void) {
    if (!g_menu.host.open) {
        return;
    }
    popup_host_close(&g_menu.host);
    g_menu.item = NULL;
    g_menu.host.output = NULL;
    g_menu.entry_count = 0;
    g_menu.hovered_index = -1;
}

bool tray_context_menu_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_menu.host, surface);
}

bool tray_context_menu_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_menu.host, surface);
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
    if (!g_menu.host.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx != g_menu.hovered_index) {
        g_menu.hovered_index = idx;
        redraw();
    }
}

void tray_context_menu_handle_click(double x, double y) {
    if (!g_menu.host.open) {
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

int tray_context_menu_test_entry_count(void) { return g_menu.host.open ? g_menu.entry_count : -1; }

const DbusMenuEntry *tray_context_menu_test_entry(int index) {
    return (g_menu.host.open && index >= 0 && index < g_menu.entry_count) ? &g_menu.entries[index] : NULL;
}

bool tray_context_menu_test_target(const char *label, double *x, double *y) {
    for (int i = 0; g_menu.host.open && i < g_menu.entry_count; i++) {
        if (!g_menu.entries[i].is_separator && strcmp(g_menu.entries[i].label, label) == 0) {
            *x = card_inner_x() + card_inner_w() / 2.0;
            *y = g_menu.rows[i].y + g_menu.rows[i].h / 2.0;
            return true;
        }
    }
    return false;
}
