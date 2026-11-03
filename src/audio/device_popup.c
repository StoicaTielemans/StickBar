#include "audio/device_popup.h"

#include <linux/input-event-codes.h> // BTN_LEFT
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/pulse_client.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "render/meter_bar.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by modules/tray/tray_context_menu.c.
#define MAX_TRACKED_OUTPUTS 8

// Mirrors audio/pulse_client.c's MAX_PULSE_NODES, but that constant isn't part
// of pulse_client.h's public surface -- redeclared locally at the same value,
// same reasoning modules/tray/tray.c already gives for its own
// MAX_TRAY_ITEMS_HINT redeclaration.
#define MAX_DEVICE_ROWS 32

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Header,
// pill, card and chip styling match the CPU/GPU/memory/media dropdowns. The
// content width is fixed, so a long device name truncates instead of
// changing the popup's size.
#define CONTENT_W 340.0
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define CARD_GAP 8.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 10.0
#define NAME_H 18.0
#define DETAIL_H 14.0
#define CONTROL_GAP 6.0
#define CHIP_W 30.0
#define CHIP_H 22.0
#define METER_H 6.0
#define METER_SEGMENTS 30
#define METER_SLOP 4.0 // extra grab room past both meter ends, so 0% and 100% are easy to hit
#define COL_GAP 10.0
#define DESC_BUF_SIZE 300

// The meter spans 0..100%, like audio/mixer_popup.c's slider: boosting past
// 100% is rarely wanted, so the full width goes to the useful range. Scroll
// can still go up to the shared PULSE_MAX_VOLUME_PERCENT; above 100% the
// meter is full and its top end turns peach.
#define METER_MAX_PERCENT 100

// Which part of a card the pointer is over, for hover and clicks.
enum { PART_NONE, PART_BODY, PART_METER, PART_MUTE };

typedef struct {
    PulseNode *node;
    double y; // card top, relative to the popup (x is always OUTER_MARGIN)
} CardLayout;

typedef struct {
    bool open;
    bool is_sink;
    Color accent;
    Output *output;
    PopupSurface *popup;
    int top_margin, right_margin; // kept from open time, for recreating the surface

    CardLayout cards[MAX_DEVICE_ROWS];
    int card_count;
    int hovered_card, hovered_part;

    // Drag state, same model as audio/mixer_popup.c: the node being dragged
    // (a pointer -- the list can change under a live refresh mid-drag) and
    // the percent last sent, drawn instead of the cached value so the meter
    // follows the pointer without waiting for the server round trip.
    PulseNode *drag_node;
    int drag_percent;

    double popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} DevicePopupState;

static DevicePopupState g_popup = { .hovered_card = -1 };

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

// The sink popup's mute-chip icon mirrors VolumeWidget.qml's own 4-level
// icon()/muteIcon() thresholds (muted/<34/<67/else); the source popup's is
// the mic's simple 2-state one -- exactly matching AudioDevicesPopup.qml's
// and MicDevicesPopup.qml's own (different!) muteIcon(node) functions, not a
// simplification of one into the other.
static uint32_t status_icon_glyph(bool is_sink, int pct, bool muted) {
    if (is_sink) {
        if (muted || pct == 0) return GLYPH_ICON_VOLUME_MUTED;
        if (pct < 34) return GLYPH_ICON_VOLUME_LOW;
        if (pct < 67) return GLYPH_ICON_VOLUME_MEDIUM;
        return GLYPH_ICON_VOLUME_HIGH;
    }
    return muted ? GLYPH_ICON_MIC_MUTED : GLYPH_ICON_MIC;
}

// Truncates `text` to the longest prefix that, with "..." appended, still
// fits within `max_w` -- mirrors the reference's `elide: Text.ElideRight`.
// Candidates are cut on UTF-8 codepoint boundaries (not raw byte counts):
// every description seen from this project's own hardware is plain ASCII,
// but it costs nothing and matches the fix modules/media needed after
// multi-byte titles produced Pango "Invalid UTF-8 string" warnings.
static void truncate_to_width(const char *text, double max_w, int font_px, bool bold,
                               char *out, size_t out_size) {
    if (text_width(text, font_px, bold) <= max_w) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    size_t len = strlen(text);
    size_t offsets[DESC_BUF_SIZE];
    size_t n = 0;
    for (size_t i = 0; i < len && n < DESC_BUF_SIZE - 1;) {
        offsets[n++] = i;
        unsigned char c = (unsigned char)text[i];
        size_t clen = 1;
        if ((c & 0xE0) == 0xC0) {
            clen = 2;
        } else if ((c & 0xF0) == 0xE0) {
            clen = 3;
        } else if ((c & 0xF8) == 0xF0) {
            clen = 4;
        }
        i += clen;
    }
    offsets[n] = len;

    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        char candidate[DESC_BUF_SIZE + 4];
        snprintf(candidate, sizeof(candidate), "%.*s...", (int)offsets[mid], text);
        if (text_width(candidate, font_px, bold) <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    snprintf(out, out_size, "%.*s...", (int)offsets[lo], text);
}

// ---- Card geometry ---------------------------------------------------------------
// Every card has the same shape, so the x positions are shared and only the
// card's own top varies.

static double card_h(void) { return CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP + CHIP_H + CARD_PAD; }
static double inner_x(void) { return OUTER_MARGIN + CARD_PAD; }
static double inner_w(void) { return CONTENT_W - CARD_PAD * 2.0; }
static double controls_y(const CardLayout *card) { return card->y + CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP; }
static double mute_x(void) { return inner_x() + inner_w() - CHIP_W; }
static double percent_col_w(void) { return text_width("200%", FONT.sm, false); }
static double meter_x(void) { return inner_x(); }
static double meter_w(void) { return mute_x() - COL_GAP - percent_col_w() - COL_GAP - meter_x(); }

// Recomputes the card list from pulse_client's live cache (already
// always-current, so there's nothing to snapshot -- unlike
// tray_context_menu.c's one-shot DBusMenu fetch). Monitor sources (a sink's
// ".monitor") are left out of the mic list: they aren't microphones, and
// with three outputs they used to outnumber the real inputs. One is still
// shown if it *is* the default, so it can be seen and switched away from.
static void compute_layout(void) {
    int count = g_popup.is_sink ? pulse_sink_count() : pulse_source_count();
    double y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    int n = 0;
    for (int i = 0; i < count && n < MAX_DEVICE_ROWS; i++) {
        PulseNode *node = g_popup.is_sink ? pulse_sink_get(i) : pulse_source_get(i);
        if (!node) {
            continue; // removed between count() and get() -- the next refresh catches up
        }
        if (!g_popup.is_sink && pulse_node_is_monitor(node) && !pulse_node_is_default(node)) {
            continue;
        }
        if (n > 0) {
            y += CARD_GAP;
        }
        g_popup.cards[n].node = node;
        g_popup.cards[n].y = y;
        y += card_h();
        n++;
    }
    g_popup.card_count = n;
    g_popup.popup_width = CONTENT_W + OUTER_MARGIN * 2.0;
    g_popup.popup_height = y + OUTER_MARGIN;
}

static int displayed_percent(PulseNode *node) {
    return node == g_popup.drag_node ? g_popup.drag_percent : pulse_node_volume_percent(node);
}

// ---- Painting ----------------------------------------------------------------

static void paint_header(cairo_t *cr) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    char icon[5], summary[48];
    glyph_utf8(g_popup.is_sink ? GLYPH_ICON_HEADPHONES : GLYPH_ICON_MIC, icon);
    const char *title = g_popup.is_sink ? "Output" : "Input";
    snprintf(summary, sizeof(summary), "  ·  %d device%s", g_popup.card_count, g_popup.card_count == 1 ? "" : "s");
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, g_popup.accent);
    double tx = x + text_width(icon, FONT.heading, false) + 8.0;
    module_box_draw_text_left(cr, tx, y, HEADER_H, title, FONT.md, true, COLORS.text);
    tx += text_width(title, FONT.md, true);
    module_box_draw_text_left(cr, tx, y, HEADER_H, summary, FONT.sm, false, COLORS.subtext0);

    // Pill: the default device's volume, or "muted". Omitted if the default
    // isn't in the list (no default reported yet).
    for (int i = 0; i < g_popup.card_count; i++) {
        PulseNode *node = g_popup.cards[i].node;
        if (!pulse_node_is_default(node)) {
            continue;
        }
        char word[16];
        bool muted = pulse_node_muted(node);
        if (muted) {
            snprintf(word, sizeof(word), "muted");
        } else {
            snprintf(word, sizeof(word), "%d%%", displayed_percent(node));
        }
        Color fill = muted ? COLORS.red : g_popup.accent;
        double pill_w = text_width(word, FONT.xs, true) + PILL_PAD_X * 2.0;
        double pill_x = x + CONTENT_W - pill_w;
        double pill_y = y + (HEADER_H - PILL_H) / 2.0;
        module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, fill, COLORS.mantle, 0.0);
        module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, word, FONT.xs, true, COLORS.mantle);
        break;
    }
}

// "USB · 48 kHz · 2 ch", skipping whatever the device doesn't report (the
// state word is drawn after it separately, in its own colour).
static void detail_text(PulseNode *node, char *out, size_t out_size) {
    char parts[4][24];
    int n = 0;
    const char *bus = pulse_node_bus(node);
    if (bus[0]) {
        snprintf(parts[n++], sizeof(parts[0]), "%s", bus);
    }
    uint32_t rate = pulse_node_rate(node);
    if (rate > 0) {
        if (rate % 1000 == 0) {
            snprintf(parts[n++], sizeof(parts[0]), "%u kHz", rate / 1000);
        } else {
            snprintf(parts[n++], sizeof(parts[0]), "%.1f kHz", rate / 1000.0);
        }
    }
    int channels = pulse_node_channels(node);
    if (channels > 0) {
        snprintf(parts[n++], sizeof(parts[0]), "%d ch", channels);
    }
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(out);
        snprintf(out + len, out_size - len, "%s%s", i > 0 ? " · " : "", parts[i]);
    }
}

static void paint_chip(cairo_t *cr, double x, double y, bool hovered, uint32_t glyph, Color color) {
    module_box_draw_rect(cr, x, y, CHIP_W, CHIP_H, hovered ? COLORS.surface1 : COLORS.surface0, COLORS.mantle, 0.0);
    char buf[5];
    glyph_utf8(glyph, buf);
    module_box_draw_text_centered(cr, x, y, CHIP_W, CHIP_H, buf, FONT.heading, false, color);
}

static void paint_card(cairo_t *cr, int i) {
    CardLayout *card = &g_popup.cards[i];
    PulseNode *node = card->node;
    bool is_default = pulse_node_is_default(node);
    bool muted = pulse_node_muted(node);
    int pct = displayed_percent(node);
    bool body_hovered = i == g_popup.hovered_card && g_popup.hovered_part == PART_BODY;

    // The default device gets the accent border, like the calendar's
    // current month; a hovered card lightens to show it's clickable.
    module_box_draw_rect(cr, OUTER_MARGIN, card->y, CONTENT_W, card_h(),
                         body_hovered && !is_default ? COLORS.surface0 : COLORS.base,
                         is_default ? g_popup.accent : COLORS.surface1, is_default ? 2.0 : 1.0);

    double y = card->y + CARD_PAD;
    const char *tag = is_default ? "default" : body_hovered ? "set default" : "";
    double tag_w = tag[0] ? text_width(tag, FONT.xs, true) + COL_GAP : 0.0;
    char name[DESC_BUF_SIZE];
    truncate_to_width(pulse_node_description(node), inner_w() - tag_w, FONT.label, true, name, sizeof(name));
    module_box_draw_text_left(cr, inner_x(), y, NAME_H, name, FONT.label, true,
                              is_default ? g_popup.accent : COLORS.text);
    if (tag[0]) {
        draw_text_right(cr, inner_x() + inner_w(), y, NAME_H, tag, FONT.xs, true,
                        is_default ? g_popup.accent : COLORS.overlay0);
    }
    y += NAME_H;

    // Detail line, with the state word in its own colour at the end.
    char detail[96];
    detail_text(node, detail, sizeof(detail));
    module_box_draw_text_left(cr, inner_x(), y, DETAIL_H, detail, FONT.xs, false, COLORS.overlay0);
    const char *state = pulse_node_state(node);
    if (state[0]) {
        Color state_color = strcmp(state, "running") == 0 ? COLORS.green
                            : strcmp(state, "idle") == 0  ? COLORS.yellow
                                                          : COLORS.overlay0;
        char tail[24];
        snprintf(tail, sizeof(tail), "%s%s", detail[0] ? " · " : "", state);
        module_box_draw_text_left(cr, inner_x() + text_width(detail, FONT.xs, false), y, DETAIL_H, tail, FONT.xs,
                                  false, state_color);
    }

    // Controls: volume meter (a slider), percent, mute chip.
    double cy = controls_y(card);
    Color gradient[3];
    if (muted) {
        gradient[0] = gradient[1] = gradient[2] = COLORS.overlay0;
    } else {
        gradient[0] = gradient[1] = g_popup.accent;
        gradient[2] = pct > METER_MAX_PERCENT ? COLORS.peach : g_popup.accent;
    }
    meter_bar_draw(cr, meter_x(), cy + (CHIP_H - METER_H) / 2.0, meter_w(), METER_H,
                   (double)pct / METER_MAX_PERCENT, METER_SEGMENTS, gradient, COLORS.surface0);
    char pct_text[16];
    snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
    draw_text_right(cr, mute_x() - COL_GAP, cy, CHIP_H, pct_text, FONT.sm, false,
                    muted ? COLORS.overlay0 : COLORS.subtext0);
    bool mute_hovered = i == g_popup.hovered_card && g_popup.hovered_part == PART_MUTE;
    paint_chip(cr, mute_x(), cy, mute_hovered, status_icon_glyph(g_popup.is_sink, pct, muted),
               muted ? COLORS.overlay0 : g_popup.accent);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, g_popup.accent, (double)BAR.module_border_width);
    paint_header(cr);
    for (int i = 0; i < g_popup.card_count; i++) {
        paint_card(cr, i);
    }
}

static void redraw(void) {
    if (g_popup.popup) {
        popup_surface_render_now(g_popup.popup, draw_popup, NULL);
    }
}

static PopupSurface *create_menu_surface(void) {
    return popup_surface_create(g_popup.output->app, g_popup.output, (int)g_popup.popup_width,
                                (int)g_popup.popup_height, g_popup.top_margin, g_popup.right_margin, 0, 0,
                                ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                                ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
}

// Called after every pulse_client_pump() so an external change (another app,
// media keys, a device plugged in) shows up live. If the card count changed
// the menu surface is recreated at the new height -- not resized in place,
// for the same reason as audio/mixer_popup.c (Hyprland unmapped a shrinking
// set_size()). A drag in progress loses its implicit grab across a
// recreate; the next release still ends it.
void device_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
    double old_h = g_popup.popup_height;
    compute_layout();
    if (g_popup.card_count == 0) {
        device_popup_close();
        return;
    }
    if (g_popup.hovered_card >= g_popup.card_count) {
        g_popup.hovered_card = -1;
        g_popup.hovered_part = PART_NONE;
    }
    if (g_popup.popup_height != old_h) {
        popup_surface_destroy(g_popup.popup);
        g_popup.popup = create_menu_surface();
        if (!g_popup.popup) {
            device_popup_close();
            return;
        }
    }
    redraw();
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

void device_popup_open(Output *output, bool is_sink, Color accent,
                        double box_x, double box_y, double box_w, double box_h) {
    device_popup_close(); // at most one device popup open at a time

    g_popup.is_sink = is_sink;
    g_popup.accent = accent;
    g_popup.hovered_card = -1;
    g_popup.hovered_part = PART_NONE;
    g_popup.drag_node = NULL;
    g_popup.output = output;

    compute_layout();
    if (g_popup.card_count == 0) {
        return; // no sinks/sources at all -- nothing to show
    }

    // Same anchor+margin math as modules/tray/tray_context_menu.c (see that
    // file for the BAR.height correction's reasoning) -- both live in the
    // bar's right region, so right-anchoring the popup under its trigger's
    // right edge reuses proven-correct positioning rather than the reference
    // QML's own left-aligned-under-widget popup.
    g_popup.top_margin = (int)(box_y + box_h) - BAR.height;
    g_popup.right_margin = output->width - (int)(box_x + box_w);
    if (g_popup.right_margin < 0) {
        g_popup.right_margin = 0;
    }
    g_popup.popup = create_menu_surface();
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
    g_popup.card_count = 0;
    g_popup.hovered_card = -1;
    g_popup.hovered_part = PART_NONE;
    g_popup.drag_node = NULL;
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

// Which card and which part of it (x, y) lands on; -1 / PART_NONE if none.
static int card_at(double x, double y, int *part) {
    *part = PART_NONE;
    if (x < OUTER_MARGIN || x >= OUTER_MARGIN + CONTENT_W) {
        return -1;
    }
    for (int i = 0; i < g_popup.card_count; i++) {
        CardLayout *card = &g_popup.cards[i];
        if (y < card->y || y >= card->y + card_h()) {
            continue;
        }
        double cy = controls_y(card);
        bool in_controls = y >= cy && y < cy + CHIP_H;
        if (in_controls && x >= mute_x() && x < mute_x() + CHIP_W) {
            *part = PART_MUTE;
        } else if (in_controls && x >= meter_x() - METER_SLOP && x < meter_x() + meter_w() + METER_SLOP) {
            *part = PART_METER;
        } else {
            *part = PART_BODY;
        }
        return i;
    }
    return -1;
}

// Maps a surface-local x to a meter percent, clamped to the track -- used
// for the press and every drag motion after it (where x can land well
// outside the track, or the popup, under the implicit grab).
static int percent_at_x(double x) {
    double frac = (x - meter_x()) / meter_w();
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return (int)lround(frac * METER_MAX_PERCENT);
}

static void drag_to(double x) {
    int pct = percent_at_x(x);
    if (pct == g_popup.drag_percent) {
        return; // don't flood the server with identical requests on every pixel
    }
    g_popup.drag_percent = pct;
    pulse_node_set_volume_percent(g_popup.drag_node, pct);
    redraw();
}

void device_popup_handle_motion(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    if (g_popup.drag_node) {
        drag_to(x);
        return;
    }
    int part;
    int idx = card_at(x, y, &part);
    if (idx != g_popup.hovered_card || part != g_popup.hovered_part) {
        g_popup.hovered_card = idx;
        g_popup.hovered_part = part;
        redraw();
    }
}

void device_popup_handle_press(double x, double y, uint32_t button) {
    if (!g_popup.open || button != BTN_LEFT) {
        return;
    }
    int part;
    int idx = card_at(x, y, &part);
    if (idx < 0) {
        return;
    }
    PulseNode *node = g_popup.cards[idx].node;
    if (part == PART_MUTE) {
        pulse_node_toggle_mute(node);
    } else if (part == PART_METER) {
        g_popup.drag_node = node;
        g_popup.drag_percent = -1; // force drag_to() to send the first value
        drag_to(x);
    } else {
        // The rest of the card sets this device as the default, matching the
        // reference's onClicked on the device name.
        pulse_node_set_default(node);
    }
    // Deliberately doesn't close the popup -- see device_popup.h.
}

void device_popup_handle_release(void) {
    if (!g_popup.drag_node) {
        return;
    }
    g_popup.drag_node = NULL;
    redraw(); // fall back to drawing the cached (server-confirmed) value
}

void device_popup_handle_scroll(double x, double y, int direction) {
    if (!g_popup.open || g_popup.drag_node) {
        return;
    }
    int part;
    int idx = card_at(x, y, &part);
    if (idx < 0) {
        return;
    }
    pulse_node_adjust_volume(g_popup.cards[idx].node,
                             direction > 0 ? PULSE_VOLUME_STEP_PERCENT : -PULSE_VOLUME_STEP_PERCENT);
}
