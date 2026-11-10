#include "modules/clock/clock_calendar_popup.h"

#include <linux/input-event-codes.h> // BTN_LEFT
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "render/color.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup's click-catcher table.
#define MAX_TRACKED_OUTPUTS 8

// Geometry. Originally mirrored the reference ClockYearPopup.qml's spacing;
// the redesign (per-month cards, week numbers, larger day cells, a header
// with navigation) moved away from that, so these are this popup's own
// values now. All local -- widget-specific tuning, not shared FONT/BAR tokens.
#define OUTER_MARGIN 14.0   // popup edge -> header / card grid
#define HEADER_PAD 4.0      // vertical padding inside the header row (hover bg)
#define HEADER_GAP 12.0     // header -> card grid
#define ARROW_BOX_W 30.0    // click/hover box around each year-navigation chevron
#define CARD_SPACING 12.0   // between month cards, both directions
#define CARD_PAD 8.0        // month card border -> its contents
#define NAME_GAP 6.0        // month name -> weekday initials
#define WEEKDAY_GAP 4.0     // weekday initials -> first week row
#define CELL_SIZE 22.0      // one day cell (was 18 with 8px numbers -- too small to read)
#define CELL_GAP 2.0        // between day cells
#define WEEKNUM_W 18.0      // ISO week-number column
#define WEEKNUM_GAP 4.0     // week-number column -> Monday column
#define PAST_ALPHA 0.4      // days before today are drawn at this opacity
#define CARD_COLUMNS 4
#define CARD_ROWS 3
#define WEEK_ROWS 6 // every month card is padded to 6 week-rows so all 12 cards
                     // line up into a uniform grid -- a deliberate simplification
                     // vs. the reference's per-card height (there, Qt's GridLayout
                     // just leaves blank space under a shorter card in the same
                     // row instead). Also keeps the popup the same size for every
                     // year, so year navigation never has to resize the surface.

static const char *MONTH_NAMES[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};
// Monday-first, matching the reference CalendarUtils.js's weekDayLabels order.
static const char WEEKDAY_INITIALS[7] = { 'M', 'T', 'W', 'T', 'F', 'S', 'S' };

typedef enum {
    HOVER_NONE,
    HOVER_PREV,  // "<" -- previous year
    HOVER_NEXT,  // ">" -- next year
    HOVER_TODAY, // the year + date text in the middle -- jumps back to this year
} HoverTarget;

typedef struct {
    double x, y, w, h;
} Rect;

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;

    // Today's date, snapshotted when the popup opens (see
    // docs/popups.md for why it isn't kept live while open).
    int year;
    int today_month0; // 0-11
    int today_mday;
    char today_label[64]; // "Tuesday, 3 November"

    int view_year; // year currently shown; changed by the header arrows
    HoverTarget hover;

    double card_w, card_h;
    double header_h, title_h, date_h, month_name_h, weekday_h, weeknum_h;
    double popup_width, popup_height;
    Rect prev_rect, next_rect, today_rect; // header hit targets, popup-local

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} CalendarPopupState;

static CalendarPopupState g_popup;

// Days in `month0` (0-11) of `year`, via mktime's own normalization of "day 0
// of the following month" rather than a hand-rolled leap-year table.
static int days_in_month(int year, int month0) {
    struct tm t = { 0 };
    t.tm_year = year - 1900;
    t.tm_mon = month0 + 1;
    t.tm_mday = 0;
    t.tm_hour = 12; // noon, to stay well clear of any DST transition at midnight
    mktime(&t);
    return t.tm_mday;
}

// 0=Monday..6=Sunday for the 1st of `month0`/`year`, converted from mktime's
// tm_wday (0=Sunday..6=Saturday) to match CalendarUtils.js's Monday-first grid.
static int first_weekday_mon0(int year, int month0) {
    struct tm t = { 0 };
    t.tm_year = year - 1900;
    t.tm_mon = month0;
    t.tm_mday = 1;
    t.tm_hour = 12;
    mktime(&t);
    return (t.tm_wday + 6) % 7;
}

// ISO 8601 week number of a date. `mday` may be out of range (0, -2, 33...):
// mktime() normalizes it into the neighbouring month/year, which is exactly
// what's needed for a week row that straddles a month boundary. strftime's %V
// does the actual ISO rule (week 1 = the week containing the year's first
// Thursday), so e.g. 1 Jan 2027 -- a Friday -- correctly reports week 53.
static int iso_week_of(int year, int month0, int mday) {
    struct tm t = { 0 };
    t.tm_year = year - 1900;
    t.tm_mon = month0;
    t.tm_mday = mday;
    t.tm_hour = 12;
    mktime(&t);
    char buf[4];
    strftime(buf, sizeof(buf), "%V", &t);
    return atoi(buf);
}

static bool is_before_today(int year, int month0, int mday) {
    if (year != g_popup.year) {
        return year < g_popup.year;
    }
    if (month0 != g_popup.today_month0) {
        return month0 < g_popup.today_month0;
    }
    return mday < g_popup.today_mday;
}

static bool rect_contains(Rect r, double x, double y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Text shown after the year: today's date when looking at this year, or a
// "back to <year>" hint when looking at another one (clicking it goes back).
static void header_date_text(char *out, size_t out_size) {
    if (g_popup.view_year == g_popup.year) {
        snprintf(out, out_size, "·  %s", g_popup.today_label);
    } else {
        snprintf(out, out_size, "·  back to %d", g_popup.year);
    }
}

// Header geometry depends on the date text, which changes with view_year, so
// it's recomputed on every draw (cheap: two text measurements).
static void layout_header(void) {
    char year_label[12];
    snprintf(year_label, sizeof(year_label), "%d", g_popup.view_year);
    char date_label[96];
    header_date_text(date_label, sizeof(date_label));

    double year_w, date_w, h;
    module_box_measure_text(year_label, FONT.title, true, &year_w, &h);
    module_box_measure_text(date_label, FONT.md, false, &date_w, &h);

    double gap = 10.0; // year -> date text
    double group_w = year_w + gap + date_w;
    double group_x = (g_popup.popup_width - group_w) / 2.0;
    double y = OUTER_MARGIN;

    g_popup.prev_rect = (Rect){ OUTER_MARGIN, y, ARROW_BOX_W, g_popup.header_h };
    g_popup.next_rect = (Rect){ g_popup.popup_width - OUTER_MARGIN - ARROW_BOX_W, y, ARROW_BOX_W, g_popup.header_h };
    g_popup.today_rect = (Rect){ group_x - 8.0, y, group_w + 16.0, g_popup.header_h };
}

static void draw_header(cairo_t *cr) {
    layout_header();

    Color none = { 0, 0, 0, 0 };
    Rect hovered = { 0 };
    if (g_popup.hover == HOVER_PREV) hovered = g_popup.prev_rect;
    if (g_popup.hover == HOVER_NEXT) hovered = g_popup.next_rect;
    if (g_popup.hover == HOVER_TODAY) hovered = g_popup.today_rect;
    if (hovered.w > 0) {
        module_box_draw_rect(cr, hovered.x, hovered.y, hovered.w, hovered.h, COLORS.surface0, none, 0.0);
    }

    char glyph[5];
    glyph_utf8(GLYPH_CHEVRON_LEFT, glyph);
    module_box_draw_text_centered(cr, g_popup.prev_rect.x, g_popup.prev_rect.y, g_popup.prev_rect.w,
                                   g_popup.prev_rect.h, glyph, FONT.md, false, COLORS.subtext1);
    glyph_utf8(GLYPH_CHEVRON_RIGHT, glyph);
    module_box_draw_text_centered(cr, g_popup.next_rect.x, g_popup.next_rect.y, g_popup.next_rect.w,
                                   g_popup.next_rect.h, glyph, FONT.md, false, COLORS.subtext1);

    char year_label[12];
    snprintf(year_label, sizeof(year_label), "%d", g_popup.view_year);
    char date_label[96];
    header_date_text(date_label, sizeof(date_label));
    double year_w, h;
    module_box_measure_text(year_label, FONT.title, true, &year_w, &h);

    double x = g_popup.today_rect.x + 8.0;
    module_box_draw_text_left(cr, x, g_popup.today_rect.y, g_popup.today_rect.h, year_label, FONT.title, true,
                               COLORS.mauve);
    module_box_draw_text_left(cr, x + year_w + 10.0, g_popup.today_rect.y, g_popup.today_rect.h, date_label,
                               FONT.md, false, COLORS.subtext0);
}

static void draw_month_card(cairo_t *cr, int m, double card_x, double card_y) {
    int year = g_popup.view_year;
    bool is_current_month = year == g_popup.year && m == g_popup.today_month0;

    // Card: base background, thin surface1 border -- or a mauve border (and a
    // mauve month name) for the month we're in, so it's findable at a glance.
    module_box_draw_rect(cr, card_x, card_y, g_popup.card_w, g_popup.card_h, COLORS.base,
                          is_current_month ? COLORS.mauve : COLORS.surface1, is_current_month ? 2.0 : 1.0);

    double inner_x = card_x + CARD_PAD;
    double y = card_y + CARD_PAD;
    double inner_w = g_popup.card_w - 2.0 * CARD_PAD;

    module_box_draw_text_centered(cr, inner_x, y, inner_w, g_popup.month_name_h, MONTH_NAMES[m], FONT.md, true,
                                   is_current_month ? COLORS.mauve : COLORS.peach);
    y += g_popup.month_name_h + NAME_GAP;

    double days_x = inner_x + WEEKNUM_W + WEEKNUM_GAP;
    for (int wd = 0; wd < 7; wd++) {
        char letter[2] = { WEEKDAY_INITIALS[wd], '\0' };
        double cell_x = days_x + wd * (CELL_SIZE + CELL_GAP);
        module_box_draw_text_centered(cr, cell_x, y, CELL_SIZE, g_popup.weekday_h, letter, FONT.sm, false,
                                       wd >= 5 ? COLORS.red : COLORS.subtext0);
    }
    y += g_popup.weekday_h + WEEKDAY_GAP;

    int start_offset = first_weekday_mon0(year, m);
    int ndays = days_in_month(year, m);

    for (int week = 0; week < WEEK_ROWS; week++) {
        double row_y = y + week * (CELL_SIZE + CELL_GAP);
        int first_day = week * 7 - start_offset + 1; // day number in this row's Monday cell
        if (first_day > ndays || first_day + 6 < 1) {
            continue; // padding row with no day of this month -- no week number either
        }

        // ISO week of this row's Thursday (the day that decides which ISO year a
        // week belongs to), even if that Thursday falls in a neighbouring month.
        char weeknum[12];
        snprintf(weeknum, sizeof(weeknum), "%d", iso_week_of(year, m, first_day + 3));
        module_box_draw_text_centered(cr, inner_x, row_y, WEEKNUM_W, CELL_SIZE, weeknum, FONT.xs, false,
                                       COLORS.overlay0);

        for (int wd = 0; wd < 7; wd++) {
            int day_num = first_day + wd;
            if (day_num < 1 || day_num > ndays) {
                continue;
            }
            double cell_x = days_x + wd * (CELL_SIZE + CELL_GAP);
            bool is_today = year == g_popup.year && m == g_popup.today_month0 && day_num == g_popup.today_mday;

            Color text_color = wd >= 5 ? COLORS.red : COLORS.text;
            if (is_before_today(year, m, day_num)) {
                text_color = color_with_alpha(text_color, PAST_ALPHA);
            }
            if (is_today) {
                cairo_save(cr);
                cairo_arc(cr, cell_x + CELL_SIZE / 2.0, row_y + CELL_SIZE / 2.0, CELL_SIZE / 2.0, 0, 2 * M_PI);
                color_set_source(cr, COLORS.mauve);
                cairo_fill(cr);
                cairo_restore(cr);
                text_color = COLORS.base;
            }

            char day_label[12]; // enough for any int, so gcc's format-truncation
                                 // analysis (which can't see days_in_month()'s
                                 // actual 1..31 range) doesn't warn
            snprintf(day_label, sizeof(day_label), "%d", day_num);
            module_box_draw_text_centered(cr, cell_x, row_y, CELL_SIZE, CELL_SIZE, day_label, FONT.sm, is_today,
                                           text_color);
        }
    }
}

static void draw_calendar(cairo_t *cr, void *user_data) {
    (void)user_data;

    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, COLORS.mauve, (double)BAR.module_border_width);

    draw_header(cr);

    double grid_top = OUTER_MARGIN + g_popup.header_h + HEADER_GAP;
    for (int m = 0; m < 12; m++) {
        int col = m % CARD_COLUMNS;
        int row = m / CARD_COLUMNS;
        double card_x = OUTER_MARGIN + col * (g_popup.card_w + CARD_SPACING);
        double card_y = grid_top + row * (g_popup.card_h + CARD_SPACING);
        draw_month_card(cr, m, card_x, card_y);
    }
}

static void rerender(void) {
    if (g_popup.open && g_popup.popup) {
        popup_surface_render_now(g_popup.popup, draw_calendar, NULL);
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

void clock_calendar_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
}

void clock_calendar_popup_toggle(Output *output, double box_x, double box_y, double box_w, double box_h) {
    if (g_popup.open) {
        clock_calendar_popup_close();
        return;
    }

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    g_popup.year = local.tm_year + 1900;
    g_popup.today_month0 = local.tm_mon;
    g_popup.today_mday = local.tm_mday;
    // The bar never calls setlocale(), so this is always the C locale's
    // English day/month names, matching the hardcoded MONTH_NAMES above.
    strftime(g_popup.today_label, sizeof(g_popup.today_label), "%A, %-d %B", &local);
    g_popup.view_year = g_popup.year; // always opens on the current year
    g_popup.hover = HOVER_NONE;
    g_popup.output = output;

    double w;
    module_box_measure_text("2026", FONT.title, true, &w, &g_popup.title_h);
    module_box_measure_text("Tuesday", FONT.md, false, &w, &g_popup.date_h);
    module_box_measure_text("January", FONT.md, true, &w, &g_popup.month_name_h);
    module_box_measure_text("M", FONT.sm, false, &w, &g_popup.weekday_h);
    (void)w;
    g_popup.header_h = (g_popup.title_h > g_popup.date_h ? g_popup.title_h : g_popup.date_h) + 2.0 * HEADER_PAD;

    g_popup.card_w = 2.0 * CARD_PAD + WEEKNUM_W + WEEKNUM_GAP + 7.0 * CELL_SIZE + 6.0 * CELL_GAP;
    g_popup.card_h = 2.0 * CARD_PAD + g_popup.month_name_h + NAME_GAP + g_popup.weekday_h + WEEKDAY_GAP +
                     WEEK_ROWS * CELL_SIZE + (WEEK_ROWS - 1) * CELL_GAP;

    g_popup.popup_width = CARD_COLUMNS * g_popup.card_w + (CARD_COLUMNS - 1) * CARD_SPACING + OUTER_MARGIN * 2.0;
    g_popup.popup_height = OUTER_MARGIN + g_popup.header_h + HEADER_GAP + CARD_ROWS * g_popup.card_h +
                            (CARD_ROWS - 1) * CARD_SPACING + OUTER_MARGIN;

    // Same anchor+margin math (including the BAR.height correction) as
    // modules/tray/tray_context_menu.c -- see
    // docs/popups.md for the reasoning. Right-anchored, unlike
    // modules/cpu/cpu_popup.c, because the clock lives in the bar's right
    // region, not the left.
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
        g_popup.output = NULL;
        return;
    }

    create_catchers(output->app);
    g_popup.open = true;
    layout_header(); // hit rects must exist before the first click/motion
    rerender();
}

void clock_calendar_popup_handle_click(double x, double y, uint32_t button) {
    if (!g_popup.open || button != BTN_LEFT) {
        return;
    }
    int before = g_popup.view_year;
    if (rect_contains(g_popup.prev_rect, x, y)) {
        g_popup.view_year--;
    } else if (rect_contains(g_popup.next_rect, x, y)) {
        g_popup.view_year++;
    } else if (rect_contains(g_popup.today_rect, x, y)) {
        g_popup.view_year = g_popup.year;
    }
    // Anything else (the month cards) is a no-op that keeps the popup open --
    // only clicks outside it, on a catcher, close it.
    if (g_popup.view_year != before) {
        rerender();
    }
}

void clock_calendar_popup_handle_motion(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    HoverTarget hover = HOVER_NONE;
    if (rect_contains(g_popup.prev_rect, x, y)) {
        hover = HOVER_PREV;
    } else if (rect_contains(g_popup.next_rect, x, y)) {
        hover = HOVER_NEXT;
    } else if (rect_contains(g_popup.today_rect, x, y)) {
        hover = HOVER_TODAY;
    }
    // Only repaint when the highlighted target actually changes, not on every
    // motion event across the popup.
    if (hover != g_popup.hover) {
        g_popup.hover = hover;
        rerender();
    }
}

bool clock_calendar_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool clock_calendar_popup_is_catcher_surface(struct wl_surface *surface) {
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
