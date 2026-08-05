#include "modules/clock/clock_calendar_popup.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "bar/popup_surface.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup's click-catcher table.
#define MAX_TRACKED_OUTPUTS 8

// Geometry constants, chosen to visually mirror the reference
// ClockYearPopup.qml (values annotated with their QML counterpart) even where
// the underlying layout mechanism differs (plain arithmetic here vs. Qt's
// GridLayout/ColumnLayout bindings there).
#define OUTER_MARGIN 12.0  // matches `implicitWidth: yearColumn.implicitWidth + 24`
#define TITLE_SPACING 10.0 // matches the yearColumn ColumnLayout's `spacing: 10`
#define CARD_SPACING_X 24.0 // matches the GridLayout's `columnSpacing: 24`
#define CARD_SPACING_Y 16.0 // matches the GridLayout's `rowSpacing: 16`
#define CARD_INNER_SPACING 4.0 // matches each month card's own ColumnLayout `spacing: 4`
#define CELL_SIZE 18.0     // matches the weekday-label `Layout.preferredWidth: 18`
                            // and day-circle `width/height: 18`
#define CARD_COLUMNS 4
#define CARD_ROWS 3
#define WEEK_ROWS 6 // every month card is padded to 6 week-rows so all 12 cards
                     // line up into a uniform grid -- a deliberate simplification
                     // vs. the reference's per-card height (there, Qt's GridLayout
                     // just leaves blank space under a shorter card in the same
                     // row instead)

static const char *MONTH_NAMES[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};
// Monday-first, matching the reference CalendarUtils.js's weekDayLabels order.
static const char WEEKDAY_INITIALS[7] = { 'M', 'T', 'W', 'T', 'F', 'S', 'S' };

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;

    int year;
    int today_month0; // 0-11
    int today_mday;

    double card_w, card_h;
    double title_h, month_name_h, weekday_h;
    double popup_width, popup_height;

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

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_calendar(cairo_t *cr, void *user_data) {
    (void)user_data;

    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, COLORS.mauve, (double)BAR.module_border_width);

    char year_label[8];
    snprintf(year_label, sizeof(year_label), "%d", g_popup.year);
    module_box_draw_text_centered(cr, 0, OUTER_MARGIN, g_popup.popup_width, g_popup.title_h,
                                   year_label, FONT.title, true, COLORS.mauve);

    double grid_top = OUTER_MARGIN + g_popup.title_h + TITLE_SPACING;

    for (int m = 0; m < 12; m++) {
        int col = m % CARD_COLUMNS;
        int row = m / CARD_COLUMNS;
        double card_x = OUTER_MARGIN + col * (g_popup.card_w + CARD_SPACING_X);
        double card_y = grid_top + row * (g_popup.card_h + CARD_SPACING_Y);

        module_box_draw_text_centered(cr, card_x, card_y, g_popup.card_w, g_popup.month_name_h,
                                       MONTH_NAMES[m], FONT.md, true, COLORS.peach);

        double weekday_y = card_y + g_popup.month_name_h + CARD_INNER_SPACING;
        for (int wd = 0; wd < 7; wd++) {
            char letter[2] = { WEEKDAY_INITIALS[wd], '\0' };
            double cell_x = card_x + wd * (CELL_SIZE + CARD_INNER_SPACING);
            module_box_draw_text_centered(cr, cell_x, weekday_y, CELL_SIZE, g_popup.weekday_h,
                                           letter, FONT.sm, false, COLORS.subtext0);
        }

        double days_top = weekday_y + g_popup.weekday_h + CARD_INNER_SPACING;
        int start_offset = first_weekday_mon0(g_popup.year, m);
        int ndays = days_in_month(g_popup.year, m);

        for (int cell = 0; cell < WEEK_ROWS * 7; cell++) {
            int day_num = cell - start_offset + 1;
            if (day_num < 1 || day_num > ndays) {
                continue;
            }
            int week = cell / 7;
            int wd = cell % 7;
            double cell_x = card_x + wd * (CELL_SIZE + CARD_INNER_SPACING);
            double cell_y = days_top + week * (CELL_SIZE + CARD_INNER_SPACING);

            bool is_today = m == g_popup.today_month0 && day_num == g_popup.today_mday;
            Color text_color = COLORS.text;
            if (is_today) {
                cairo_save(cr);
                cairo_arc(cr, cell_x + CELL_SIZE / 2.0, cell_y + CELL_SIZE / 2.0, CELL_SIZE / 2.0, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, COLORS.mauve.r, COLORS.mauve.g, COLORS.mauve.b, COLORS.mauve.a);
                cairo_fill(cr);
                cairo_restore(cr);
                text_color = COLORS.base;
            }

            char day_label[12]; // enough for any int, so gcc's format-truncation
                                 // analysis (which can't see days_in_month()'s
                                 // actual 1..31 range) doesn't warn
            snprintf(day_label, sizeof(day_label), "%d", day_num);
            module_box_draw_text_centered(cr, cell_x, cell_y, CELL_SIZE, CELL_SIZE,
                                           day_label, FONT.xs, is_today, text_color);
        }
    }
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
    g_popup.output = output;

    char year_label[8];
    snprintf(year_label, sizeof(year_label), "%d", g_popup.year);
    double w;
    module_box_measure_text(year_label, FONT.title, true, &w, &g_popup.title_h);
    module_box_measure_text("January", FONT.md, true, &w, &g_popup.month_name_h);
    module_box_measure_text("M", FONT.sm, false, &w, &g_popup.weekday_h);
    (void)w;

    g_popup.card_w = 7.0 * CELL_SIZE + 6.0 * CARD_INNER_SPACING;
    g_popup.card_h = g_popup.month_name_h + CARD_INNER_SPACING + g_popup.weekday_h + CARD_INNER_SPACING +
                     WEEK_ROWS * CELL_SIZE + (WEEK_ROWS - 1) * CARD_INNER_SPACING;

    g_popup.popup_width = CARD_COLUMNS * g_popup.card_w + (CARD_COLUMNS - 1) * CARD_SPACING_X + OUTER_MARGIN * 2.0;
    g_popup.popup_height = g_popup.title_h + TITLE_SPACING + CARD_ROWS * g_popup.card_h +
                            (CARD_ROWS - 1) * CARD_SPACING_Y + OUTER_MARGIN * 2.0;

    // Same anchor+margin math (including the BAR.height correction) as
    // modules/tray/tray_context_menu.c -- see CLAUDE.md's "D-Bus / system
    // tray" section for the reasoning. Right-anchored, unlike
    // sysmon/sysmon_info_popup.c, because the clock lives in the bar's right
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
    popup_surface_render_now(g_popup.popup, draw_calendar, NULL);
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
