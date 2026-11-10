#include "modules/privacy/privacy_info_popup.h"

#include <stdio.h>

#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "pipewire/pipewire_client.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"

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
#define CARD_PAD 8.0
#define CARD_TITLE_H 18.0
#define CARD_TITLE_ICON_GAP 6.0
#define APP_ROW_H 18.0

typedef struct {
    PrivacyCategory category;
    char name[APP_NAME_LEN];
} PrivacyEntry;

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    PrivacyEntry entries[MAX_PRIVACY_ENTRIES];
    int entry_count;
    double content_w, popup_width, popup_height;

} InfoPopupState;

static InfoPopupState g_popup;

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
        double needed = module_box_text_width(g_popup.entries[i].name, FONT.md, false) + CARD_PAD * 2.0;
        if (needed > w) {
            w = needed;
        }
    }
    g_popup.content_w = w > MAX_CONTENT_W ? MAX_CONTENT_W : w;
    g_popup.popup_width = g_popup.content_w + POPUP_OUTER_MARGIN * 2.0;

    double y = POPUP_OUTER_MARGIN + POPUP_HEADER_H + POPUP_SECTION_GAP;
    bool any = false;
    for (int cat = 0; cat < CATEGORY_COUNT; cat++) {
        int n = category_entry_count((PrivacyCategory)cat);
        if (n > 0) {
            y += (any ? POPUP_CARD_GAP : 0.0) + CARD_PAD * 2.0 + CARD_TITLE_H + n * APP_ROW_H;
            any = true;
        }
    }
    g_popup.popup_height = y + POPUP_OUTER_MARGIN;
}

static void paint_header(cairo_t *cr) {
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    char icon[5], summary[32];
    glyph_utf8(GLYPH_ICON_PRIVACY, icon);
    snprintf(summary, sizeof(summary), "  ·  %d stream%s", g_popup.entry_count, g_popup.entry_count == 1 ? "" : "s");
    PopupHeader header = {.icon = icon, .icon_color = COLORS.maroon, .title = "Privacy", .summary = summary,
                          .pill = "in use", .pill_fill = COLORS.maroon};
    popup_draw_header(cr, x, y, g_popup.content_w, &header);
}

// Returns the card's bottom edge.
static double paint_category_card(cairo_t *cr, PrivacyCategory category, int count, double y) {
    double h = CARD_PAD * 2.0 + CARD_TITLE_H + count * APP_ROW_H;
    popup_draw_card(cr, POPUP_OUTER_MARGIN, y, g_popup.content_w, h, false, false, COLORS.surface1);

    double inner_x = POPUP_OUTER_MARGIN + CARD_PAD;
    double inner_w = g_popup.content_w - CARD_PAD * 2.0;
    double row_y = y + CARD_PAD;
    char icon[5], count_text[16];
    glyph_utf8(category_glyph(category), icon);
    module_box_draw_text_left(cr, inner_x, row_y, CARD_TITLE_H, icon, FONT.md, false, COLORS.maroon);
    module_box_draw_text_left(cr, inner_x + module_box_text_width(icon, FONT.md, false) + CARD_TITLE_ICON_GAP, row_y,
                              CARD_TITLE_H,
                              category_title(category), FONT.xs, true, COLORS.overlay0);
    snprintf(count_text, sizeof(count_text), "%d", count);
    module_box_draw_text_right(cr, inner_x + inner_w, row_y, CARD_TITLE_H, count_text, FONT.xs, true, COLORS.overlay0);
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
    popup_draw_frame(cr, g_popup.popup_width, g_popup.popup_height, COLORS.maroon);
    paint_header(cr);
    double y = POPUP_OUTER_MARGIN + POPUP_HEADER_H + POPUP_SECTION_GAP;
    for (int cat = 0; cat < CATEGORY_COUNT; cat++) {
        int n = category_entry_count((PrivacyCategory)cat);
        if (n > 0) {
            y = paint_category_card(cr, (PrivacyCategory)cat, n, y) + POPUP_CARD_GAP;
        }
    }
}

void privacy_info_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    privacy_info_popup_close(); // at most one privacy info popup open at a time

    g_popup.host.output = output;
    snapshot_entries();
    compute_layout();

    // Under its box, opening rightward from the box's left edge (bar/popup_host.c).
    if (!popup_host_open(&g_popup.host, output, POPUP_ANCHOR_LEFT, box_x, box_y, box_w, box_h,
                         (int)g_popup.popup_width, (int)g_popup.popup_height)) {
        return;
    }
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void privacy_info_popup_close(void) {
    if (!g_popup.host.open) {
        return;
    }
    popup_host_close(&g_popup.host);
    g_popup.entry_count = 0;
}

bool privacy_info_popup_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_popup.host, surface);
}

bool privacy_info_popup_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_popup.host, surface);
}

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

int privacy_info_popup_test_entry_count(void) { return g_popup.host.open ? g_popup.entry_count : -1; }

const char *privacy_info_popup_test_entry_name(int i) { return g_popup.entries[i].name; }

const char *privacy_info_popup_test_entry_category(int i) { return category_title(g_popup.entries[i].category); }
