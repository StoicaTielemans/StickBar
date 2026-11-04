#include "modules/privacy/privacy_info_popup.h"

#include <stdio.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "pipewire/pipewire_client.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other per-output popup table in this project.
#define MAX_TRACKED_OUTPUTS 8

// How many streams one dropdown lists -- same "documented cap, not dynamic
// growth" convention as every other fixed-array table in this project.
#define MAX_PRIVACY_ENTRIES 32
#define APP_NAME_LEN 128
#define CATEGORY_COUNT 3

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Header,
// pill and card styling match the other dropdowns on purpose. Unlike them the
// width follows the longest app name (between the two limits), since the
// content is snapshotted at open and so can't change size while it's shown.
#define MIN_CONTENT_W 260.0
#define MAX_CONTENT_W 480.0
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define CARD_GAP 8.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 8.0
#define CARD_TITLE_H 18.0
#define CARD_TITLE_ICON_GAP 6.0
#define APP_ROW_H 18.0

typedef struct {
    PrivacyCategory category;
    char name[APP_NAME_LEN];
} PrivacyEntry;

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;
    PrivacyEntry entries[MAX_PRIVACY_ENTRIES];
    int entry_count;
    double content_w, popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} InfoPopupState;

static InfoPopupState g_popup;

static double text_width(const char *text, int size, bool bold) {
    double w, h;
    module_box_measure_text(text, size, bold, &w, &h);
    (void)h;
    return w;
}

static void draw_text_right(cairo_t *cr, double right_x, double y, double h, const char *text, int size, bool bold,
                            Color color) {
    module_box_draw_text_left(cr, right_x - text_width(text, size, bold), y, h, text, size, bold, color);
}

static const char *category_title(PrivacyCategory category) {
    switch (category) {
    case PRIVACY_CATEGORY_MIC: return "MICROPHONE";
    case PRIVACY_CATEGORY_WEBCAM: return "WEBCAM";
    case PRIVACY_CATEGORY_SCREENSHARE: return "SCREEN SHARE";
    }
    return "";
}

static uint32_t category_glyph(PrivacyCategory category) {
    switch (category) {
    case PRIVACY_CATEGORY_MIC: return GLYPH_ICON_MIC;
    case PRIVACY_CATEGORY_WEBCAM: return GLYPH_ICON_WEBCAM;
    case PRIVACY_CATEGORY_SCREENSHARE: return GLYPH_ICON_SCREENSHARE;
    }
    return GLYPH_ICON_PRIVACY;
}

static int category_entry_count(PrivacyCategory category) {
    int n = 0;
    for (int i = 0; i < g_popup.entry_count; i++) {
        if (g_popup.entries[i].category == category) {
            n++;
        }
    }
    return n;
}

// Snapshots the streams in a fixed MIC/WEBCAM/SCREEN SHARE order -- the enum's
// order, not the order they happen to be stored in pipewire_client's node table.
static void snapshot_entries(void) {
    g_popup.entry_count = 0;
    int count = pipewire_client_entry_count();
    for (int cat = 0; cat < CATEGORY_COUNT; cat++) {
        for (int i = 0; i < count && g_popup.entry_count < MAX_PRIVACY_ENTRIES; i++) {
            PrivacyEntry *entry = &g_popup.entries[g_popup.entry_count];
            if (pipewire_client_entry(i, &entry->category, entry->name, sizeof(entry->name)) &&
                entry->category == (PrivacyCategory)cat) {
                g_popup.entry_count++;
            }
        }
    }
}

static void compute_layout(void) {
    double w = MIN_CONTENT_W;
    for (int i = 0; i < g_popup.entry_count; i++) {
        double needed = text_width(g_popup.entries[i].name, FONT.md, false) + CARD_PAD * 2.0;
        if (needed > w) {
            w = needed;
        }
    }
    g_popup.content_w = w > MAX_CONTENT_W ? MAX_CONTENT_W : w;
    g_popup.popup_width = g_popup.content_w + OUTER_MARGIN * 2.0;

    double y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    bool any = false;
    for (int cat = 0; cat < CATEGORY_COUNT; cat++) {
        int n = category_entry_count((PrivacyCategory)cat);
        if (n > 0) {
            y += (any ? CARD_GAP : 0.0) + CARD_PAD * 2.0 + CARD_TITLE_H + n * APP_ROW_H;
            any = true;
        }
    }
    g_popup.popup_height = y + OUTER_MARGIN;
}

static void paint_header(cairo_t *cr) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    char icon[5], summary[32];
    glyph_utf8(GLYPH_ICON_PRIVACY, icon);
    snprintf(summary, sizeof(summary), "  ·  %d stream%s", g_popup.entry_count, g_popup.entry_count == 1 ? "" : "s");
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, COLORS.maroon);
    double tx = x + text_width(icon, FONT.heading, false) + 8.0;
    module_box_draw_text_left(cr, tx, y, HEADER_H, "Privacy", FONT.md, true, COLORS.text);
    tx += text_width("Privacy", FONT.md, true);
    module_box_draw_text_left(cr, tx, y, HEADER_H, summary, FONT.sm, false, COLORS.subtext0);

    const char *word = "in use";
    double pill_w = text_width(word, FONT.xs, true) + PILL_PAD_X * 2.0;
    double pill_x = x + g_popup.content_w - pill_w;
    double pill_y = y + (HEADER_H - PILL_H) / 2.0;
    module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, COLORS.maroon, COLORS.mantle, 0.0);
    module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, word, FONT.xs, true, COLORS.mantle);
}

// Returns the card's bottom edge.
static double paint_category_card(cairo_t *cr, PrivacyCategory category, int count, double y) {
    double h = CARD_PAD * 2.0 + CARD_TITLE_H + count * APP_ROW_H;
    module_box_draw_rect(cr, OUTER_MARGIN, y, g_popup.content_w, h, COLORS.base, COLORS.surface1, 1.0);

    double inner_x = OUTER_MARGIN + CARD_PAD;
    double inner_w = g_popup.content_w - CARD_PAD * 2.0;
    double row_y = y + CARD_PAD;
    char icon[5], count_text[16];
    glyph_utf8(category_glyph(category), icon);
    module_box_draw_text_left(cr, inner_x, row_y, CARD_TITLE_H, icon, FONT.md, false, COLORS.maroon);
    module_box_draw_text_left(cr, inner_x + text_width(icon, FONT.md, false) + CARD_TITLE_ICON_GAP, row_y, CARD_TITLE_H,
                              category_title(category), FONT.xs, true, COLORS.overlay0);
    snprintf(count_text, sizeof(count_text), "%d", count);
    draw_text_right(cr, inner_x + inner_w, row_y, CARD_TITLE_H, count_text, FONT.xs, true, COLORS.overlay0);
    row_y += CARD_TITLE_H;

    // Clipped to the card: a name past MAX_CONTENT_W is cut off at the edge
    // rather than drawn over the popup's border.
    cairo_save(cr);
    cairo_rectangle(cr, inner_x, y, inner_w, h);
    cairo_clip(cr);
    for (int i = 0; i < g_popup.entry_count; i++) {
        if (g_popup.entries[i].category != category) {
            continue;
        }
        module_box_draw_text_left(cr, inner_x, row_y, APP_ROW_H, g_popup.entries[i].name, FONT.md, false, COLORS.text);
        row_y += APP_ROW_H;
    }
    cairo_restore(cr);
    return y + h;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, COLORS.maroon, (double)BAR.module_border_width);
    paint_header(cr);
    double y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    for (int cat = 0; cat < CATEGORY_COUNT; cat++) {
        int n = category_entry_count((PrivacyCategory)cat);
        if (n > 0) {
            y = paint_category_card(cr, (PrivacyCategory)cat, n, y) + CARD_GAP;
        }
    }
}

static void create_catchers(AppContext *app) {
    g_popup.catcher_count = 0;
    for (Output *o = app->outputs; o && g_popup.catcher_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create_catcher(app, o);
        if (!catcher) {
            continue;
        }
        g_popup.catchers[g_popup.catcher_count] = catcher;
        g_popup.catcher_surfaces[g_popup.catcher_count] = popup_surface_wl_surface(catcher);
        g_popup.catcher_count++;
    }
}

static void destroy_catchers(void) {
    for (int i = 0; i < g_popup.catcher_count; i++) {
        popup_surface_destroy(g_popup.catchers[i]);
        g_popup.catchers[i] = NULL;
        g_popup.catcher_surfaces[i] = NULL;
    }
    g_popup.catcher_count = 0;
}

void privacy_info_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    (void)box_w; // left-anchored: only the box's left edge (box_x) matters, not its width
    privacy_info_popup_close(); // at most one privacy info popup open at a time

    g_popup.output = output;
    snapshot_entries();
    compute_layout();

    // Same top_margin math (including the BAR.height correction) as
    // modules/cpu/cpu_popup.c -- see docs/popups.md
    // for the original reasoning. Anchored TOP+LEFT with a
    // left_margin, since the privacy module lives in the bar's left region
    // like CPU/GPU/memory.
    int top_margin = (int)(box_y + box_h) - BAR.height;
    int left_margin = (int)box_x;

    g_popup.popup = popup_surface_create(output->app, output, (int)g_popup.popup_width, (int)g_popup.popup_height,
                                          top_margin, 0, 0, left_margin,
                                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
                                          ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    if (!g_popup.popup) {
        return;
    }

    create_catchers(output->app);
    g_popup.open = true;
    popup_surface_render_now(g_popup.popup, draw_popup, NULL);
}

void privacy_info_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
    g_popup.entry_count = 0;
}

bool privacy_info_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool privacy_info_popup_is_catcher_surface(struct wl_surface *surface) {
    if (!g_popup.open) {
        return false;
    }
    for (int i = 0; i < g_popup.catcher_count; i++) {
        if (g_popup.catcher_surfaces[i] == surface) {
            return true;
        }
    }
    return false;
}

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

int privacy_info_popup_test_entry_count(void) { return g_popup.open ? g_popup.entry_count : -1; }

const char *privacy_info_popup_test_entry_name(int i) { return g_popup.entries[i].name; }

const char *privacy_info_popup_test_entry_category(int i) { return category_title(g_popup.entries[i].category); }
