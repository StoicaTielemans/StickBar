#include "audio/device_popup.h"

#include <stdio.h>
#include <string.h>

#include "audio/pulse_client.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by modules/tray/tray_context_menu.c.
#define MAX_TRACKED_OUTPUTS 8

// Mirrors audio/pulse_client.c's MAX_PULSE_NODES, but that constant isn't part
// of pulse_client.h's public surface -- redeclared locally at the same value,
// same reasoning modules/tray/tray.c already gives for its own
// MAX_TRAY_ITEMS_HINT redeclaration.
#define MAX_DEVICE_ROWS 32

// Geometry constants, chosen to visually mirror the reference
// AudioDevicesPopup.qml / MicDevicesPopup.qml (values annotated with their QML
// counterpart), matching modules/tray/tray_context_menu.c's own convention of
// keeping this widget-specific tuning local rather than in config.h.
#define ROW_SPACING 8.0        // matches the ColumnLayout's `spacing: 8`
#define ROW_INNER_SPACING 10.0 // matches each RowLayout's `spacing: 10`
#define OUTER_MARGIN 8.0       // matches `Config.bar.popupPadding`
#define PERCENT_COL_W 40.0     // matches `Layout.preferredWidth: 40`
#define DESC_MAX_W 260.0       // matches `Layout.maximumWidth: 260`
#define DESC_BUF_SIZE 300
#define PCT_BUF_SIZE 16

typedef struct {
    double y, h; // x is always OUTER_MARGIN, width is always g_popup.content_width
} RowLayout;

typedef struct {
    bool open;
    bool is_sink;
    Color accent;
    Output *output;
    PopupSurface *popup;

    PulseNode *nodes[MAX_DEVICE_ROWS];
    RowLayout rows[MAX_DEVICE_ROWS];
    char desc_text[MAX_DEVICE_ROWS][DESC_BUF_SIZE]; // pre-truncated, computed at layout time
    char pct_text[MAX_DEVICE_ROWS][PCT_BUF_SIZE];
    int row_count;
    int hovered_index;

    double star_col_w, mute_col_w, content_width, popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} DevicePopupState;

static DevicePopupState g_popup = { .hovered_index = -1 };

// The sink popup's row icon mirrors VolumeWidget.qml's own 4-level
// icon()/muteIcon() thresholds (muted/<34/<67/else); the source popup's row
// icon is the mic's simple 2-state one -- exactly matching
// AudioDevicesPopup.qml's and MicDevicesPopup.qml's own (different!)
// muteIcon(node) functions, not a simplification of one into the other.
static uint32_t row_status_icon_glyph(bool is_sink, PulseNode *node) {
    bool muted = pulse_node_muted(node);
    int pct = pulse_node_volume_percent(node);
    if (is_sink) {
        if (muted || pct == 0) return GLYPH_ICON_VOLUME_MUTED;
        if (pct < 34) return GLYPH_ICON_VOLUME_LOW;
        if (pct < 67) return GLYPH_ICON_VOLUME_MEDIUM;
        return GLYPH_ICON_VOLUME_HIGH;
    }
    return muted ? GLYPH_ICON_MIC_MUTED : GLYPH_ICON_MIC;
}

// Truncates `text` (byte-based -- every real device description seen from
// this project's actual PulseAudio/PipeWire hardware is plain ASCII, e.g.
// "Burr-Brown from TI USB Audio CODEC Analog Stereo", so this doesn't need to
// be UTF-8-codepoint-aware) to the longest prefix that, with "..." appended,
// still fits within `max_w` at the given font -- mirrors the reference's
// `elide: Text.ElideRight` + `Layout.maximumWidth: 260`.
static void truncate_to_width(const char *text, double max_w, int font_px, bool bold,
                               char *out, size_t out_size) {
    double w, h;
    module_box_measure_text(text, font_px, bold, &w, &h);
    if (w <= max_w) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    size_t len = strlen(text);
    size_t lo = 0, hi = len;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        char candidate[DESC_BUF_SIZE];
        snprintf(candidate, sizeof(candidate), "%.*s...", (int)mid, text);
        module_box_measure_text(candidate, font_px, bold, &w, &h);
        if (w <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    snprintf(out, out_size, "%.*s...", (int)lo, text);
}

// Recomputes row geometry (and the truncated description/percent strings
// drawn from) directly from pulse_client's live cache -- unlike
// tray_context_menu.c, which snapshots DBusMenu entries because those come
// from a one-shot async D-Bus fetch, pulse_client's sink/source lists are
// already a live, always-current local cache, so there's nothing to snapshot:
// this can just be re-run on every open *and* every live update while open.
static void compute_layout(void) {
    char buf[5];

    glyph_utf8(GLYPH_ICON_DEVICE_DEFAULT, buf);
    double star_w1, star_h1;
    module_box_measure_text(buf, FONT.heading, false, &star_w1, &star_h1);
    glyph_utf8(GLYPH_ICON_DEVICE_SELECTABLE, buf);
    double star_w2, star_h2;
    module_box_measure_text(buf, FONT.heading, false, &star_w2, &star_h2);
    double star_w = star_w1 > star_w2 ? star_w1 : star_w2;
    double star_h = star_h1 > star_h2 ? star_h1 : star_h2;

    // Widest of every possible status-icon glyph for this popup's kind, so the
    // mute-icon column is a fixed width regardless of which glyph a given row
    // ends up needing (sink rows can show any of 4; source rows, 2).
    static const uint32_t SINK_ICONS[4] = { GLYPH_ICON_VOLUME_MUTED, GLYPH_ICON_VOLUME_LOW,
                                             GLYPH_ICON_VOLUME_MEDIUM, GLYPH_ICON_VOLUME_HIGH };
    static const uint32_t SOURCE_ICONS[2] = { GLYPH_ICON_MIC_MUTED, GLYPH_ICON_MIC };
    const uint32_t *icons = g_popup.is_sink ? SINK_ICONS : SOURCE_ICONS;
    int icon_count = g_popup.is_sink ? 4 : 2;
    double mute_w = 0, mute_h = 0;
    for (int i = 0; i < icon_count; i++) {
        glyph_utf8(icons[i], buf);
        double w, h;
        module_box_measure_text(buf, FONT.heading, false, &w, &h);
        if (w > mute_w) mute_w = w;
        if (h > mute_h) mute_h = h;
    }

    g_popup.star_col_w = star_w;
    g_popup.mute_col_w = mute_w;

    int count = g_popup.is_sink ? pulse_sink_count() : pulse_source_count();
    if (count > MAX_DEVICE_ROWS) {
        count = MAX_DEVICE_ROWS; // documented limitation, matches sni_registry's own cap behavior
    }
    g_popup.row_count = count;

    char prefix_buf[5];
    glyph_utf8(g_popup.is_sink ? GLYPH_ICON_HEADPHONES : GLYPH_ICON_MIC, prefix_buf);

    double max_content_w = 0;
    double row_heights[MAX_DEVICE_ROWS];

    for (int i = 0; i < count; i++) {
        PulseNode *node = g_popup.is_sink ? pulse_sink_get(i) : pulse_source_get(i);
        g_popup.nodes[i] = node;

        char full_desc[DESC_BUF_SIZE];
        snprintf(full_desc, sizeof(full_desc), "%s %s", prefix_buf, pulse_node_description(node));
        truncate_to_width(full_desc, DESC_MAX_W, FONT.label, false, g_popup.desc_text[i], DESC_BUF_SIZE);

        double desc_w, desc_h;
        module_box_measure_text(g_popup.desc_text[i], FONT.label, false, &desc_w, &desc_h);

        snprintf(g_popup.pct_text[i], PCT_BUF_SIZE, "%d%%", pulse_node_volume_percent(node));
        double pct_w, pct_h;
        module_box_measure_text(g_popup.pct_text[i], FONT.sm, false, &pct_w, &pct_h);

        double row_content_w =
            star_w + ROW_INNER_SPACING + desc_w + ROW_INNER_SPACING + PERCENT_COL_W + ROW_INNER_SPACING + mute_w;
        if (row_content_w > max_content_w) {
            max_content_w = row_content_w;
        }

        double row_h = star_h;
        if (desc_h > row_h) row_h = desc_h;
        if (pct_h > row_h) row_h = pct_h;
        if (mute_h > row_h) row_h = mute_h;
        row_heights[i] = row_h;
    }

    g_popup.content_width = max_content_w;
    g_popup.popup_width = max_content_w + OUTER_MARGIN * 2.0;

    double y = OUTER_MARGIN;
    for (int i = 0; i < count; i++) {
        g_popup.rows[i].y = y;
        g_popup.rows[i].h = row_heights[i];
        y += row_heights[i];
        if (i < count - 1) {
            y += ROW_SPACING;
        }
    }
    g_popup.popup_height = y + OUTER_MARGIN;
}

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_row(cairo_t *cr, int i) {
    PulseNode *node = g_popup.nodes[i];
    RowLayout *row = &g_popup.rows[i];
    bool is_default = pulse_node_is_default(node);
    bool muted = pulse_node_muted(node);

    if (i == g_popup.hovered_index) {
        module_box_draw_rect(cr, OUTER_MARGIN, row->y, g_popup.content_width, row->h,
                              COLORS.surface1, (Color){ 0, 0, 0, 0 }, 0.0);
    }

    double star_x = OUTER_MARGIN;
    double desc_x = OUTER_MARGIN + g_popup.star_col_w + ROW_INNER_SPACING;
    double mute_x = OUTER_MARGIN + g_popup.content_width - g_popup.mute_col_w;
    double percent_x = mute_x - ROW_INNER_SPACING - PERCENT_COL_W;

    char glyph_buf[5];
    glyph_utf8(is_default ? GLYPH_ICON_DEVICE_DEFAULT : GLYPH_ICON_DEVICE_SELECTABLE, glyph_buf);
    module_box_draw_text_left(cr, star_x, row->y, row->h, glyph_buf, FONT.heading, false, g_popup.accent);

    Color desc_color = is_default ? g_popup.accent : COLORS.text;
    module_box_draw_text_left(cr, desc_x, row->y, row->h, g_popup.desc_text[i], FONT.label, is_default, desc_color);

    double pct_w, pct_h;
    module_box_measure_text(g_popup.pct_text[i], FONT.sm, false, &pct_w, &pct_h);
    (void)pct_h;
    // Right-aligned within the PERCENT_COL_W column -- reuses draw_text_left
    // with a precomputed x rather than adding a new right-align helper, same
    // trick module_box.c's existing two draw functions already make possible.
    module_box_draw_text_left(cr, percent_x + (PERCENT_COL_W - pct_w), row->y, row->h,
                               g_popup.pct_text[i], FONT.sm, false, COLORS.subtext0);

    glyph_utf8(row_status_icon_glyph(g_popup.is_sink, node), glyph_buf);
    Color mute_color = muted ? COLORS.overlay0 : g_popup.accent;
    module_box_draw_text_left(cr, mute_x, row->y, row->h, glyph_buf, FONT.heading, false, mute_color);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, g_popup.accent, (double)BAR.module_border_width);
    for (int i = 0; i < g_popup.row_count; i++) {
        draw_row(cr, i);
    }
}

static void redraw(void) {
    if (g_popup.popup) {
        popup_surface_render_now(g_popup.popup, draw_popup, NULL);
    }
}

// Called by modules/volume and modules/mic (via their pulse_client notify-fd
// handling in main.c) whenever the audio cache changes while this popup is
// open, so e.g. an external volume change (another app, physical media keys)
// shows up live instead of only reflecting state as of the moment it opened.
void device_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
    compute_layout();
    redraw();
}

static void create_catchers(AppContext *app) {
    g_popup.catcher_count = 0;
    for (Output *o = app->outputs; o && g_popup.catcher_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create(
            app, o, 0, 0, 0, 0, 0, 0,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP);
        if (!catcher) {
            continue;
        }
        popup_surface_render_now(catcher, draw_transparent, NULL);
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

void device_popup_open(Output *output, bool is_sink, Color accent,
                        double box_x, double box_y, double box_w, double box_h) {
    device_popup_close(); // at most one device popup open at a time

    g_popup.is_sink = is_sink;
    g_popup.accent = accent;
    g_popup.hovered_index = -1;
    g_popup.output = output;

    compute_layout();
    if (g_popup.row_count == 0) {
        return; // no sinks/sources at all -- nothing to show
    }

    // Same anchor+margin math as modules/tray/tray_context_menu.c (see that
    // file for the BAR.height correction's reasoning) -- both live in the
    // bar's right region, so right-anchoring the popup under its trigger's
    // right edge reuses proven-correct positioning rather than the reference
    // QML's own left-aligned-under-widget popup.
    int top_margin = (int)(box_y + box_h) - BAR.height;
    int right_margin = output->width - (int)(box_x + box_w);
    if (right_margin < 0) {
        right_margin = 0;
    }

    g_popup.popup = popup_surface_create(output->app, output, (int)g_popup.popup_width, (int)g_popup.popup_height,
                                          top_margin, right_margin, 0, 0,
                                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                                          ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    if (!g_popup.popup) {
        return;
    }

    create_catchers(output->app);
    g_popup.open = true;
    redraw();
}

void device_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
    g_popup.row_count = 0;
    g_popup.hovered_index = -1;
}

bool device_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool device_popup_is_catcher_surface(struct wl_surface *surface) {
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

static int row_at(double x, double y) {
    if (x < OUTER_MARGIN || x >= OUTER_MARGIN + g_popup.content_width) {
        return -1;
    }
    for (int i = 0; i < g_popup.row_count; i++) {
        RowLayout *row = &g_popup.rows[i];
        if (y >= row->y && y < row->y + row->h) {
            return i;
        }
    }
    return -1;
}

void device_popup_handle_motion(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx != g_popup.hovered_index) {
        g_popup.hovered_index = idx;
        redraw();
    }
}

void device_popup_handle_click(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx < 0) {
        return;
    }

    double mute_x = OUTER_MARGIN + g_popup.content_width - g_popup.mute_col_w;
    PulseNode *node = g_popup.nodes[idx];

    if (x >= mute_x) {
        // Landed on the mute-icon column.
        pulse_node_toggle_mute(node);
    } else {
        // Landed on the star or description column -- both set this row's
        // device as the default, matching the reference's identical
        // onClicked handler on both of those StyledText elements.
        pulse_node_set_default(node);
    }
    // Deliberately doesn't close the popup -- see device_popup.h.
}
