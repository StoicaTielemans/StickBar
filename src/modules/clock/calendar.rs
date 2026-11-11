//! The year calendar, opened by right-clicking the clock (the C bar's
//! `modules/clock/clock_calendar_popup.c`; its look is described in
//! `docs/modules/clock.md`).
//!
//! A 4x3 grid of month cards under a header: `‹` and `›` step the shown year,
//! and the text between them (the year plus today's date, or "back to <this
//! year>" when another year is shown) jumps back to the current year. Today
//! is a filled mauve circle, earlier days are faded, weekends are red, and
//! each week row starts with its ISO week number. Every month card is padded
//! to six week rows, so all twelve line up in a uniform grid and the popup is
//! the same size for every year.
//!
//! The date is taken when the popup opens (it's short-lived), and it always
//! opens on the current year.

use std::fs::File;

use chrono::{Datelike, Local, NaiveDate, TimeDelta};

use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{
        COLORS, Color, FONT,
        glyphs::{ICON_CHEVRON_LEFT, ICON_CHEVRON_RIGHT},
    },
    render::{BoxRect, module_box::draw_rect, popup_chrome, text::TextCache, whole_px},
    wayland::BTN_LEFT,
};

/// Popup edge to header and card grid. Wider than other popups' 12px margin.
const OUTER_MARGIN: f64 = 14.0;
/// Vertical padding inside the header row (its hover background).
const HEADER_PAD: f64 = 4.0;
/// Header to card grid.
const HEADER_GAP: f64 = 12.0;
/// The click and hover box around each year chevron.
const ARROW_BOX_W: f64 = 30.0;
/// Between the year and the date text in the header.
const YEAR_DATE_GAP: f64 = 10.0;
/// Extra hover/click area each side of the header's middle text.
const TODAY_PAD: f64 = 8.0;
/// Between month cards, both directions.
const CARD_SPACING: f64 = 12.0;
/// Month card border to its contents.
const CARD_PAD: f64 = 8.0;
/// Month name to weekday initials.
const NAME_GAP: f64 = 6.0;
/// Weekday initials to the first week row.
const WEEKDAY_GAP: f64 = 4.0;
/// One day cell (square).
const CELL_SIZE: f64 = 22.0;
/// Between day cells.
const CELL_GAP: f64 = 2.0;
/// The ISO week-number column.
const WEEKNUM_W: f64 = 18.0;
/// Week-number column to the Monday column.
const WEEKNUM_GAP: f64 = 4.0;
/// Days before today are drawn at this opacity.
const PAST_ALPHA: f64 = 0.4;
/// Month cards per grid row.
const CARD_COLUMNS: u32 = 4;
/// Grid rows of month cards.
const CARD_ROWS: u32 = 3;
/// Week rows per month card (the most any month needs).
const WEEK_ROWS: i32 = 6;

/// Month names, January first.
const MONTH_NAMES: [&str; 12] = [
    "January",
    "February",
    "March",
    "April",
    "May",
    "June",
    "July",
    "August",
    "September",
    "October",
    "November",
    "December",
];
/// Weekday column headings, Monday first.
const WEEKDAY_INITIALS: [&str; 7] = ["M", "T", "W", "T", "F", "S", "S"];

/// Which header target the pointer is over.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Hover {
    /// None of them.
    Nothing,
    /// `‹`, the previous year.
    Prev,
    /// `›`, the next year.
    Next,
    /// The year and date text: back to the current year.
    Today,
}

/// Sizes measured once at open, from the fonts.
#[derive(Clone, Copy, Debug)]
struct Layout {
    /// The whole popup.
    width: f64,
    /// The whole popup.
    height: f64,
    /// The header row, padding included.
    header_h: f64,
    /// One month card.
    card_w: f64,
    /// One month card.
    card_h: f64,
    /// A month name's text height.
    month_name_h: f64,
    /// A weekday initial's text height.
    weekday_h: f64,
}

/// What the calendar shows, and its header's hit targets.
#[derive(Debug)]
struct View {
    /// Today, when the popup opened.
    today: NaiveDate,
    /// e.g. "Tuesday, 3 November".
    today_label: String,
    /// The year shown, changed by the chevrons.
    year: i32,
    /// Which header target is highlighted.
    hover: Hover,
    /// Measured sizes.
    layout: Layout,
    /// `‹` hit box, popup-local.
    prev_rect: BoxRect,
    /// `›` hit box.
    next_rect: BoxRect,
    /// The middle text's hit box.
    today_rect: BoxRect,
}

/// The calendar dropdown: closed (`host` is `None`) or open.
#[derive(Default)]
pub(crate) struct CalendarPopup {
    /// The open popup's surfaces.
    host: Option<PopupHost>,
    /// What it shows; `None` while closed.
    view: Option<View>,
}

impl CalendarPopup {
    /// Opens the calendar under the clock box `trigger`, or closes it if open.
    pub(crate) fn toggle<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        output: (
            &smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput,
            i32,
        ),
        trigger: BoxRect,
        all_outputs: impl Iterator<
            Item = &'o smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput,
        >,
        text: &mut TextCache,
    ) {
        if self.host.is_some() {
            self.close();
            return;
        }
        let now = Local::now();
        let mut view = View::new(now.date_naive(), now.format("%A, %-d %B").to_string(), text);
        view.layout_header(text);
        let size = (whole_px(view.layout.width), whole_px(view.layout.height));
        self.host = Some(PopupHost::open_right(
            env,
            output,
            trigger,
            size,
            all_outputs,
        ));
        self.view = Some(view);
        self.paint(text);
    }

    /// Closes the calendar (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.host = None;
        self.view = None;
    }

    /// The open popup's surfaces, if open.
    pub(crate) const fn host(&self) -> Option<&PopupHost> {
        self.host.as_ref()
    }

    /// The open popup's surfaces, for configure routing.
    pub(crate) const fn host_mut(&mut self) -> Option<&mut PopupHost> {
        self.host.as_mut()
    }

    /// A click on the popup, at popup-local (x, y): steps the year or returns
    /// to the current one. Only the left button does anything.
    pub(crate) fn click(&mut self, x: f64, y: f64, button: u32, text: &mut TextCache) {
        let Some(view) = self.view.as_mut() else {
            return;
        };
        if button != BTN_LEFT {
            return;
        }
        let before = view.year;
        match view.target_at(x, y) {
            Hover::Prev => view.year -= 1,
            Hover::Next => view.year += 1,
            Hover::Today => view.year = view.today.year(),
            Hover::Nothing => {}
        }
        if view.year != before {
            self.paint(text);
        }
    }

    /// Whether (x, y) is on something clickable (a chevron or the year text),
    /// for the hand cursor.
    pub(crate) fn is_target(&self, x: f64, y: f64) -> bool {
        self.view
            .as_ref()
            .is_some_and(|view| view.target_at(x, y) != Hover::Nothing)
    }

    /// The pointer moved over the popup (or left it: pass a point outside):
    /// highlights the header target under it.
    pub(crate) fn motion(&mut self, x: f64, y: f64, text: &mut TextCache) {
        let Some(view) = self.view.as_mut() else {
            return;
        };
        let hover = view.target_at(x, y);
        if hover != view.hover {
            view.hover = hover;
            self.paint(text);
        }
    }

    /// Paints the open popup (also what a menu configure asks for: see
    /// `PopupSurface`'s first-paint notes).
    pub(crate) fn paint(&mut self, text: &mut TextCache) {
        let (Some(host), Some(view)) = (self.host.as_mut(), self.view.as_mut()) else {
            return;
        };
        host.menu_mut().paint(|cr| view.draw(cr, text));
    }

    /// Writes exactly what the open popup shows to a PNG at `path`, for the
    /// tests' `snapshot` command: the same drawing, onto a fresh image.
    pub(crate) fn snapshot(&mut self, path: &str, text: &mut TextCache) -> Result<(), String> {
        let (Some(host), Some(view)) = (self.host.as_ref(), self.view.as_mut()) else {
            return Err("no open menu to snapshot".to_owned());
        };
        let (width, height) = host.menu().menu_size().ok_or("not a menu")?;
        let image =
            render_image(width, height, |cr| view.draw(cr, text)).map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }

    /// The menu surface, if open.
    pub(crate) fn menu(&self) -> Option<&PopupSurface> {
        self.host.as_ref().map(PopupHost::menu)
    }
}

impl View {
    /// The view for a popup opened on `today`, with sizes measured from the fonts.
    fn new(today: NaiveDate, today_label: String, text: &mut TextCache) -> Self {
        let (_, title_h) = text.size("2026", FONT.title, true);
        let (_, date_h) = text.size("Tuesday", FONT.md, false);
        let (_, month_name_h) = text.size("January", FONT.md, true);
        let (_, weekday_h) = text.size("M", FONT.sm, false);
        let header_h = title_h.max(date_h) + 2.0 * HEADER_PAD;

        let card_w = 2.0 * CARD_PAD + WEEKNUM_W + WEEKNUM_GAP + 7.0 * CELL_SIZE + 6.0 * CELL_GAP;
        let rows = f64::from(WEEK_ROWS);
        let card_h = 2.0 * CARD_PAD
            + month_name_h
            + NAME_GAP
            + weekday_h
            + WEEKDAY_GAP
            + rows * CELL_SIZE
            + (rows - 1.0) * CELL_GAP;
        let (columns, card_rows) = (f64::from(CARD_COLUMNS), f64::from(CARD_ROWS));
        let width = columns * card_w + (columns - 1.0) * CARD_SPACING + OUTER_MARGIN * 2.0;
        let height = OUTER_MARGIN
            + header_h
            + HEADER_GAP
            + card_rows * card_h
            + (card_rows - 1.0) * CARD_SPACING
            + OUTER_MARGIN;

        let empty = BoxRect {
            x: 0.0,
            y: 0.0,
            w: 0.0,
            h: 0.0,
        };
        Self {
            today,
            today_label,
            year: today.year(),
            hover: Hover::Nothing,
            layout: Layout {
                width,
                height,
                header_h,
                card_w,
                card_h,
                month_name_h,
                weekday_h,
            },
            prev_rect: empty,
            next_rect: empty,
            today_rect: empty,
        }
    }

    /// The header's middle text after the year.
    fn date_label(&self) -> String {
        if self.year == self.today.year() {
            format!("\u{b7}  {}", self.today_label)
        } else {
            format!("\u{b7}  back to {}", self.today.year())
        }
    }

    /// Places the header's three hit targets for the shown year (the middle
    /// text's width depends on it).
    fn layout_header(&mut self, text: &mut TextCache) {
        let (year_w, _) = text.size(&self.year.to_string(), FONT.title, true);
        let (date_w, _) = text.size(&self.date_label(), FONT.md, false);
        let group_w = year_w + YEAR_DATE_GAP + date_w;
        let group_x = (self.layout.width - group_w) / 2.0;
        let (y, h) = (OUTER_MARGIN, self.layout.header_h);
        self.prev_rect = BoxRect {
            x: OUTER_MARGIN,
            y,
            w: ARROW_BOX_W,
            h,
        };
        self.next_rect = BoxRect {
            x: self.layout.width - OUTER_MARGIN - ARROW_BOX_W,
            y,
            w: ARROW_BOX_W,
            h,
        };
        self.today_rect = BoxRect {
            x: group_x - TODAY_PAD,
            y,
            w: group_w + 2.0 * TODAY_PAD,
            h,
        };
    }

    /// Which header target (x, y) is on.
    fn target_at(&self, x: f64, y: f64) -> Hover {
        if self.prev_rect.contains(x, y) {
            Hover::Prev
        } else if self.next_rect.contains(x, y) {
            Hover::Next
        } else if self.today_rect.contains(x, y) {
            Hover::Today
        } else {
            Hover::Nothing
        }
    }

    /// Draws the whole popup.
    fn draw(&mut self, cr: &cairo::Context, text: &mut TextCache) -> Result<(), cairo::Error> {
        popup_chrome::draw_frame(cr, self.layout.width, self.layout.height, COLORS.mauve)?;
        self.draw_header(cr, text)?;
        let grid_top = OUTER_MARGIN + self.layout.header_h + HEADER_GAP;
        for (month, name) in (1_u32..).zip(MONTH_NAMES) {
            let (col, row) = ((month - 1) % CARD_COLUMNS, (month - 1) / CARD_COLUMNS);
            let card = BoxRect {
                x: OUTER_MARGIN + f64::from(col) * (self.layout.card_w + CARD_SPACING),
                y: grid_top + f64::from(row) * (self.layout.card_h + CARD_SPACING),
                w: self.layout.card_w,
                h: self.layout.card_h,
            };
            self.draw_month(cr, text, month, name, card)?;
        }
        Ok(())
    }

    /// The header: hover background, chevrons, year and date text.
    fn draw_header(
        &mut self,
        cr: &cairo::Context,
        text: &mut TextCache,
    ) -> Result<(), cairo::Error> {
        self.layout_header(text);
        let hovered = match self.hover {
            Hover::Prev => Some(self.prev_rect),
            Hover::Next => Some(self.next_rect),
            Hover::Today => Some(self.today_rect),
            Hover::Nothing => None,
        };
        if let Some(rect) = hovered {
            draw_rect(cr, rect, COLORS.surface0, Color::TRANSPARENT, 0.0)?;
        }
        let chevron = |c: char| c.to_string();
        text.draw_centered(
            cr,
            self.prev_rect,
            &chevron(ICON_CHEVRON_LEFT),
            FONT.md,
            false,
            COLORS.subtext1,
        )?;
        text.draw_centered(
            cr,
            self.next_rect,
            &chevron(ICON_CHEVRON_RIGHT),
            FONT.md,
            false,
            COLORS.subtext1,
        )?;

        let year = self.year.to_string();
        let (year_w, _) = text.size(&year, FONT.title, true);
        let row = (
            self.today_rect.x + TODAY_PAD,
            self.today_rect.y,
            self.today_rect.h,
        );
        text.draw_left(cr, row, &year, FONT.title, true, COLORS.mauve)?;
        let date_row = (row.0 + year_w + YEAR_DATE_GAP, row.1, row.2);
        text.draw_left(
            cr,
            date_row,
            &self.date_label(),
            FONT.md,
            false,
            COLORS.subtext0,
        )
    }

    /// One month's card: name, weekday initials, then up to six week rows.
    fn draw_month(
        &self,
        cr: &cairo::Context,
        text: &mut TextCache,
        month: u32,
        name: &str,
        card: BoxRect,
    ) -> Result<(), cairo::Error> {
        let year = self.year;
        let is_current_month = year == self.today.year() && month == self.today.month();
        popup_chrome::draw_card(cr, card, is_current_month, false, COLORS.mauve)?;

        let inner_x = card.x + CARD_PAD;
        let mut y = card.y + CARD_PAD;
        let name_rect = BoxRect {
            x: inner_x,
            y,
            w: card.w - 2.0 * CARD_PAD,
            h: self.layout.month_name_h,
        };
        let name_color = if is_current_month {
            COLORS.mauve
        } else {
            COLORS.peach
        };
        text.draw_centered(cr, name_rect, name, FONT.md, true, name_color)?;
        y += self.layout.month_name_h + NAME_GAP;

        let days_x = inner_x + WEEKNUM_W + WEEKNUM_GAP;
        let cell_x = |weekday: u32| days_x + f64::from(weekday) * (CELL_SIZE + CELL_GAP);
        for (weekday, letter) in (0_u32..).zip(WEEKDAY_INITIALS) {
            let rect = BoxRect {
                x: cell_x(weekday),
                y,
                w: CELL_SIZE,
                h: self.layout.weekday_h,
            };
            let color = if weekday >= 5 {
                COLORS.red
            } else {
                COLORS.subtext0
            };
            text.draw_centered(cr, rect, letter, FONT.sm, false, color)?;
        }
        y += self.layout.weekday_h + WEEKDAY_GAP;

        let start_offset = first_weekday(year, month);
        let ndays = days_in_month(year, month);
        for week in 0..WEEK_ROWS {
            let row_y = y + f64::from(week) * (CELL_SIZE + CELL_GAP);
            // The day number in this row's Monday cell (below 1 = last month).
            let first_day = week * 7 - start_offset + 1;
            if first_day > ndays || first_day + 6 < 1 {
                continue; // a padding row: no day of this month, no week number
            }
            // A week's ISO number is decided by its Thursday, which may fall in
            // the neighbouring month or year (1 Jan 2027, a Friday, is week 53).
            let weeknum = BoxRect {
                x: inner_x,
                y: row_y,
                w: WEEKNUM_W,
                h: CELL_SIZE,
            };
            let number = iso_week(year, month, first_day + 3).to_string();
            text.draw_centered(cr, weeknum, &number, FONT.xs, false, COLORS.overlay0)?;

            for weekday in 0_u32..7 {
                let day = first_day + i32::try_from(weekday).unwrap_or(0);
                if day < 1 || day > ndays {
                    continue;
                }
                let cell = BoxRect {
                    x: cell_x(weekday),
                    y: row_y,
                    w: CELL_SIZE,
                    h: CELL_SIZE,
                };
                self.draw_day(cr, text, (month, day), weekday >= 5, cell)?;
            }
        }
        Ok(())
    }

    /// One day cell: today is a filled circle, past days fade, weekends are red.
    fn draw_day(
        &self,
        cr: &cairo::Context,
        text: &mut TextCache,
        (month, day): (u32, i32),
        weekend: bool,
        cell: BoxRect,
    ) -> Result<(), cairo::Error> {
        let today = (
            self.today.year(),
            self.today.month(),
            i32::try_from(self.today.day()).unwrap_or(0),
        );
        let shown = (self.year, month, day);
        let is_today = shown == today;
        let mut color = if weekend { COLORS.red } else { COLORS.text };
        if shown < today {
            color = color.with_alpha(PAST_ALPHA);
        }
        if is_today {
            cr.save()?;
            let r = CELL_SIZE / 2.0;
            cr.arc(cell.x + r, cell.y + r, r, 0.0, std::f64::consts::TAU);
            COLORS.mauve.set_source(cr);
            cr.fill()?;
            cr.restore()?;
            color = COLORS.base;
        }
        text.draw_centered(cr, cell, &day.to_string(), FONT.sm, is_today, color)
    }
}

/// The first day of `month` (1-12) in `year`.
fn first_of_month(year: i32, month: u32) -> Option<NaiveDate> {
    NaiveDate::from_ymd_opt(year, month, 1)
}

/// How many days `month` (1-12) of `year` has.
fn days_in_month(year: i32, month: u32) -> i32 {
    let next = if month == 12 {
        first_of_month(year + 1, 1)
    } else {
        first_of_month(year, month + 1)
    };
    first_of_month(year, month)
        .zip(next)
        .and_then(|(first, next)| i32::try_from((next - first).num_days()).ok())
        .unwrap_or(0)
}

/// The weekday of the 1st of `month` (1-12), Monday = 0 ... Sunday = 6.
fn first_weekday(year: i32, month: u32) -> i32 {
    first_of_month(year, month)
        .and_then(|d| i32::try_from(d.weekday().num_days_from_monday()).ok())
        .unwrap_or(0)
}

/// The ISO week number of day `day` of `month`. `day` may be outside the
/// month (0 = the last day of the previous month, and so on), which counts on
/// into the neighbouring month or year.
fn iso_week(year: i32, month: u32, day: i32) -> u32 {
    first_of_month(year, month)
        .and_then(|first| first.checked_add_signed(TimeDelta::days(i64::from(day) - 1)))
        .map_or(0, |date| date.iso_week().week())
}

#[cfg(test)]
mod tests {
    use super::{days_in_month, first_weekday, iso_week};

    #[test]
    fn month_lengths() {
        assert_eq!(days_in_month(2028, 2), 29); // leap year
        assert_eq!(days_in_month(2026, 2), 28);
        assert_eq!(days_in_month(2026, 12), 31); // December rolls into the next year
    }

    #[test]
    fn first_weekday_is_monday_based() {
        assert_eq!(first_weekday(2026, 11), 6); // 1 Nov 2026 is a Sunday
        assert_eq!(first_weekday(2026, 6), 0); // 1 Jun 2026 is a Monday
    }

    #[test]
    fn iso_weeks_cross_years() {
        assert_eq!(iso_week(2027, 1, 1), 53); // Friday 1 Jan 2027 belongs to 2026's week 53
        assert_eq!(iso_week(2027, 1, 4), 1);
        assert_eq!(iso_week(2026, 1, 0), 1); // 31 Dec 2025 (day 0): week 1 of 2026
    }
}
