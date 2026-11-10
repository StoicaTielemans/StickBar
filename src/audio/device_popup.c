#include "audio/device_popup.h"

#include <linux/input-event-codes.h> // BTN_LEFT
#include <stdio.h>
#include <string.h>

#include "audio/pulse_client.h"
#include "audio/volume_style.h"
#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"

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
#define CARD_PAD 10.0
#define NAME_H 18.0
#define DETAIL_H 14.0
#define CONTROL_GAP 6.0
#define CHIP_W 30.0
#define CHIP_H 22.0
#define METER_H 6.0
#define METER_SEGMENTS 30
#define METER_SLOP 4.0 // extra grab room past both meter ends, so 0% and 100% are easy to hit
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
    double y; // card top, relative to the popup (x is always POPUP_OUTER_MARGIN)
} CardLayout;

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    bool is_sink;
    Color accent;

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

} DevicePopupState;

static DevicePopupState g_popup = { .hovered_card = -1 };

// The sink popup's mute-chip icon mirrors VolumeWidget.qml's own 4-level
// icon()/muteIcon() thresholds (muted/<34/<67/else); the source popup's is
// the mic's simple 2-state one -- exactly matching AudioDevicesPopup.qml's
// and MicDevicesPopup.qml's own (different!) muteIcon(node) functions, not a
// simplification of one into the other.
static uint32_t status_icon_glyph(bool is_sink, int pct, bool muted) {
    if (is_sink) {
        return volume_level_glyph(pct, muted);
    }
    return muted ? GLYPH_ICON_MIC_MUTED : GLYPH_ICON_MIC;
}

// ---- Card geometry ---------------------------------------------------------------
// Every card has the same shape, so the x positions are shared and only the
// card's own top varies.

static double card_h(void) { return CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP + CHIP_H + CARD_PAD; }
static double inner_x(void) { return POPUP_OUTER_MARGIN + CARD_PAD; }
static double inner_w(void) { return CONTENT_W - CARD_PAD * 2.0; }
static double controls_y(const CardLayout *card) { return card->y + CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP; }
static double mute_x(void) { return inner_x() + inner_w() - CHIP_W; }
static double percent_col_w(void) { return module_box_text_width("200%", FONT.sm, false); }
static double meter_x(void) { return inner_x(); }
static double meter_w(void) { return mute_x() - POPUP_COL_GAP - percent_col_w() - POPUP_COL_GAP - meter_x(); }

// Recomputes the card list from pulse_client's live cache (already
// always-current, so there's nothing to snapshot -- unlike
// tray_context_menu.c's one-shot DBusMenu fetch). Monitor sources (a sink's
// ".monitor") are left out of the mic list: they aren't microphones, and
// with three outputs they used to outnumber the real inputs. One is still
// shown if it *is* the default, so it can be seen and switched away from.
static void compute_layout(void) {
    int count = g_popup.is_sink ? pulse_sink_count() : pulse_source_count();
    double y = POPUP_OUTER_MARGIN + POPUP_HEADER_H + POPUP_SECTION_GAP;
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
            y += POPUP_CARD_GAP;
        }
        g_popup.cards[n].node = node;
        g_popup.cards[n].y = y;
        y += card_h();
        n++;
    }
    g_popup.card_count = n;
    g_popup.popup_width = CONTENT_W + POPUP_OUTER_MARGIN * 2.0;
    g_popup.popup_height = y + POPUP_OUTER_MARGIN;
}

static int displayed_percent(PulseNode *node) {
    return node == g_popup.drag_node ? g_popup.drag_percent : pulse_node_volume_percent(node);
}

// ---- Painting ----------------------------------------------------------------

static void paint_header(cairo_t *cr) {
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    char icon[5], summary[48];
    glyph_utf8(g_popup.is_sink ? GLYPH_ICON_HEADPHONES : GLYPH_ICON_MIC, icon);
    const char *title = g_popup.is_sink ? "Output" : "Input";
    snprintf(summary, sizeof(summary), "  ·  %d device%s", g_popup.card_count, g_popup.card_count == 1 ? "" : "s");
    PopupHeader header = {.icon = icon, .icon_color = g_popup.accent, .title = title, .summary = summary};

    // Pill: the default device's volume, or "muted". Omitted if the default
    // isn't in the list (no default reported yet).
    char word[16];
    for (int i = 0; i < g_popup.card_count; i++) {
        PulseNode *node = g_popup.cards[i].node;
        if (!pulse_node_is_default(node)) {
            continue;
        }
        bool muted = pulse_node_muted(node);
        if (muted) {
            snprintf(word, sizeof(word), "muted");
        } else {
            snprintf(word, sizeof(word), "%d%%", displayed_percent(node));
        }
        header.pill = word;
        header.pill_fill = muted ? COLORS.red : g_popup.accent;
        break;
    }
    popup_draw_header(cr, x, y, CONTENT_W, &header);
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
    popup_draw_card(cr, POPUP_OUTER_MARGIN, card->y, CONTENT_W, card_h(), is_default, body_hovered && !is_default,
                    g_popup.accent);

    double y = card->y + CARD_PAD;
    const char *tag = is_default ? "default" : body_hovered ? "set default" : "";
    double tag_w = tag[0] ? module_box_text_width(tag, FONT.xs, true) + POPUP_COL_GAP : 0.0;
    char name[DESC_BUF_SIZE];
    module_box_truncate_to_width(pulse_node_description(node), inner_w() - tag_w, FONT.label, true, name, sizeof(name));
    module_box_draw_text_left(cr, inner_x(), y, NAME_H, name, FONT.label, true,
                              is_default ? g_popup.accent : COLORS.text);
    if (tag[0]) {
        module_box_draw_text_right(cr, inner_x() + inner_w(), y, NAME_H, tag, FONT.xs, true,
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
        module_box_draw_text_left(cr, inner_x() + module_box_text_width(detail, FONT.xs, false), y, DETAIL_H, tail,
                                  FONT.xs,
                                  false, state_color);
    }

    // Controls: volume meter (a slider), percent, mute chip.
    double cy = controls_y(card);
    Color gradient[3];
    volume_meter_gradient(gradient, g_popup.accent, muted, pct);
    meter_bar_draw(cr, meter_x(), cy + (CHIP_H - METER_H) / 2.0, meter_w(), METER_H,
                   (double)pct / METER_MAX_PERCENT, METER_SEGMENTS, gradient, COLORS.surface0);
    char pct_text[16];
    snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
    module_box_draw_text_right(cr, mute_x() - POPUP_COL_GAP, cy, CHIP_H, pct_text, FONT.sm, false,
                               muted ? COLORS.overlay0 : COLORS.subtext0);
    bool mute_hovered = i == g_popup.hovered_card && g_popup.hovered_part == PART_MUTE;
    paint_chip(cr, mute_x(), cy, mute_hovered, status_icon_glyph(g_popup.is_sink, pct, muted),
               muted ? COLORS.overlay0 : g_popup.accent);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    popup_draw_frame(cr, g_popup.popup_width, g_popup.popup_height, g_popup.accent);
    paint_header(cr);
    for (int i = 0; i < g_popup.card_count; i++) {
        paint_card(cr, i);
    }
}

static void redraw(void) {
    if (g_popup.host.popup) {
        popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
    }
}

// Called after every pulse_client_pump() so an external change (another app,
// media keys, a device plugged in) shows up live. If the card count changed
// the menu surface is recreated at the new height -- not resized in place,
// for the same reason as audio/mixer_popup.c (Hyprland unmapped a shrinking
// set_size()). A drag in progress loses its implicit grab across a
// recreate; the next release still ends it.
void device_popup_refresh_if_open(void) {
    if (!g_popup.host.open) {
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
        if (!popup_host_resize(&g_popup.host, (int)g_popup.popup_width, (int)g_popup.popup_height)) {
            device_popup_close();
            return;
        }
    }
    redraw();
}

void device_popup_open(Output *output, bool is_sink, Color accent,
                        double box_x, double box_y, double box_w, double box_h) {
    device_popup_close(); // at most one device popup open at a time

    g_popup.is_sink = is_sink;
    g_popup.accent = accent;
    g_popup.hovered_card = -1;
    g_popup.hovered_part = PART_NONE;
    g_popup.drag_node = NULL;
    g_popup.host.output = output;

    compute_layout();
    if (g_popup.card_count == 0) {
        return; // no sinks/sources at all -- nothing to show
    }

    // Under its box, opening leftward from the box's right edge (bar/popup_host.c).
    if (!popup_host_open(&g_popup.host, output, POPUP_ANCHOR_RIGHT, box_x, box_y, box_w, box_h,
                         (int)g_popup.popup_width,
                         (int)g_popup.popup_height)) {
        return;
    }
    redraw();
}

void device_popup_close(void) {
    if (!g_popup.host.open) {
        return;
    }
    popup_host_close(&g_popup.host);
    g_popup.card_count = 0;
    g_popup.hovered_card = -1;
    g_popup.hovered_part = PART_NONE;
    g_popup.drag_node = NULL;
}

bool device_popup_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_popup.host, surface);
}

bool device_popup_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_popup.host, surface);
}

// Which card and which part of it (x, y) lands on; -1 / PART_NONE if none.
static int card_at(double x, double y, int *part) {
    *part = PART_NONE;
    if (x < POPUP_OUTER_MARGIN || x >= POPUP_OUTER_MARGIN + CONTENT_W) {
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
    return volume_slider_percent(x, meter_x(), meter_w(), METER_MAX_PERCENT);
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
    if (!g_popup.host.open) {
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
    if (!g_popup.host.open || button != BTN_LEFT) {
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
    if (!g_popup.host.open || g_popup.drag_node) {
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

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

int device_popup_test_card_count(void) { return g_popup.host.open ? g_popup.card_count : -1; }

bool device_popup_test_target(const char *description, const char *part, double frac, double *x, double *y) {
    if (!g_popup.host.open) {
        return false;
    }
    for (int i = 0; i < g_popup.card_count; i++) {
        CardLayout *card = &g_popup.cards[i];
        if (strcmp(pulse_node_description(card->node), description) != 0) {
            continue;
        }
        double cy = controls_y(card) + CHIP_H / 2.0;
        if (strcmp(part, "meter") == 0) {
            *x = meter_x() + frac * meter_w();
            *y = cy;
        } else if (strcmp(part, "mute") == 0) {
            *x = mute_x() + CHIP_W / 2.0;
            *y = cy;
        } else { // "body": the name line
            *x = inner_x() + 20.0;
            *y = card->y + CARD_PAD + NAME_H / 2.0;
        }
        return true;
    }
    return false;
}
