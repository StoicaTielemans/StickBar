//! The clock box, e.g. `󰃮 11/11/26 󰥔 18:40` (the C bar's `modules/clock/clock.c`).
//!
//! The text is formatted once per tick and cached; drawing just reads it. A
//! tick only reports a change when the text actually changed, which without
//! seconds is once a minute, so the bar commits once a minute.

pub(crate) mod timer;

use chrono::{Datelike, Local, NaiveDateTime, Timelike};

use crate::{
    config::{
        COLORS,
        glyphs::{ICON_CALENDAR, ICON_CLOCK},
    },
    render::{BoxRect, module_box, text::TextCache},
};

/// Brings the seconds back (`hh:mm:ss`) so the clock tests get a change every
/// second to measure (`test_clock_alignment`, `test_redraw`). Read once.
const SHOW_SECONDS_VAR: &str = "STICKBAR_TEST_CLOCK_SECONDS";

/// The clock module's state.
#[derive(Debug)]
pub(crate) struct Clock {
    /// The text currently shown.
    text: String,
    /// Whether seconds are shown (test mode only).
    show_seconds: bool,
}

impl Clock {
    /// A clock already showing the current time, so the first frame has it.
    pub(crate) fn new() -> Self {
        // Set at all = on, whatever the value (C checks `getenv() != NULL`).
        let mut clock = Self {
            text: String::new(),
            show_seconds: std::env::var_os(SHOW_SECONDS_VAR).is_some(),
        };
        let _changed: bool = clock.tick();
        clock
    }

    /// Re-reads the wall clock. Returns true if the shown text changed, so
    /// the caller only repaints then.
    pub(crate) fn tick(&mut self) -> bool {
        let next = format_time(Local::now().naive_local(), self.show_seconds);
        if next == self.text {
            return false;
        }
        self.text = next;
        true
    }

    /// Draws the clock box with its right edge at `right_x`, spanning
    /// `top..top + height`. Returns where the box landed.
    pub(crate) fn render(
        &self,
        cr: &cairo::Context,
        text: &mut TextCache,
        right_x: f64,
        (top, height): (f64, f64),
    ) -> Result<BoxRect, cairo::Error> {
        module_box::label(cr, text, right_x, (top, height), &self.text, COLORS.mauve)
    }
}

/// The reference bar's format minus seconds: `{cal} dd/mm/yy {clock} hh:mm`,
/// plus `:ss` when asked for.
fn format_time(now: NaiveDateTime, seconds: bool) -> String {
    let text = format!(
        "{ICON_CALENDAR} {:02}/{:02}/{:02} {ICON_CLOCK} {:02}:{:02}",
        now.day(),
        now.month(),
        // Two-digit year, like the reference's `getFullYear() % 100`.
        now.year().rem_euclid(100),
        now.hour(),
        now.minute(),
    );
    if seconds {
        format!("{text}:{:02}", now.second())
    } else {
        text
    }
}

#[cfg(test)]
mod tests {
    use chrono::NaiveDate;

    use super::format_time;

    /// 3 November 2026, 09:05:07: every field below 10, so zero-padding shows.
    fn sample() -> chrono::NaiveDateTime {
        NaiveDate::from_ymd_opt(2026, 11, 3)
            .and_then(|d| d.and_hms_opt(9, 5, 7))
            .expect("valid date")
    }

    #[test]
    fn formats_like_the_c_bar() {
        assert_eq!(
            format_time(sample(), false),
            "\u{f00ee} 03/11/26 \u{f0954} 09:05"
        );
    }

    #[test]
    fn seconds_only_in_test_mode() {
        assert_eq!(
            format_time(sample(), true),
            "\u{f00ee} 03/11/26 \u{f0954} 09:05:07"
        );
    }
}
