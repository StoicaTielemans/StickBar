# Clock and the year calendar

Files: `src/modules/clock/mod.rs` (`Clock`: formats and caches the text, draws the box),
`src/modules/clock/timer.rs` (the tick), `src/modules/clock/calendar.rs` (`CalendarPopup`).

## The box and its tick

- **The tick is a `CLOCK_REALTIME` timerfd armed `ABSTIME | CANCEL_ON_SET`** at the next
  whole second, 1 s interval, through rustix's safe wrappers (calloop's `Timer` is
  monotonic-only). Setting the wall clock (NTP step, manual change, resume) fails the next
  read with `ECANCELED`, and the timer re-arms on the new second. `test_clock_alignment`
  checks the first commit of each second lands within 50 ms of the boundary (measured: under
  1 ms). The `ECANCELED` path isn't exercised by a test (it needs root to set the clock).
- **The clock shows no seconds, so it repaints once a minute.** The timer still fires every
  second (the CPU dropdown's first figures ride on it), but `Clock::tick` returns whether the
  text changed, and only then are bars marked. Measured against a seconds build: commits
  143 -> 61/min, idle CPU 0.19 % -> 0.10 %. `STICKBAR_TEST_CLOCK_SECONDS` brings the seconds
  back, for the two tests that need a change every second.
- **A tick is a clock-only redraw** (`Dirty::Clock`): the bar copies its previous frame and
  repaints just the clock's box (`Renderer::render_clock_only`). It falls back to a full
  redraw if one is pending anyway, there's no previous frame, or the clock box came out a
  different width (the boxes next to it would move).

## The calendar

The reference's `ClockWidget.qml` shows a week strip on hover and a 12-month year grid
(`ClockYearPopup.qml`) on left-click. This bar has only the year grid (the week strip needs
hover, [popups.md](../popups.md)), toggled by a **right**-click.

- **Date maths with chrono's `NaiveDate`** (`days_in_month`, `first_weekday`, `iso_week`,
  unit-tested). ISO week numbers come from each row's *Thursday*, which decides a week's ISO
  year, so straddling rows are right (1 Jan 2027, a Friday, is week 53).
- **Every month card is 6 week-rows tall**, unlike the reference's per-month heights, so the
  12 cards line up in a uniform grid with plain arithmetic.
- **Header**: `‹`/`›` chevrons (`ICON_CHEVRON_LEFT/RIGHT`) flip the year; between them the
  year (mauve, `FONT.title`) and `· Tuesday, 3 November` (today), or `· back to 2026` when
  another year is shown, which jumps back when clicked. The three targets get a `surface0`
  hover background. It always opens on the current year.
- **Month cards**: `base` with a 1 px `surface1` border; the current month (current year
  only) gets a 2 px mauve border and a mauve name.
- **ISO week numbers** in a faint left column (`overlay0`, `FONT.xs`), only on rows with a day
  of that month.
- **Days**: `FONT.sm` in 22 px cells. Weekends (and the S/S initials) are red; days before
  today are drawn at 40 % alpha (every day of a past year, none of a future one); today is a
  filled mauve circle with bold text.
- About 880×680 px, right-anchored under the clock, the same size for every year, so
  flipping years only repaints, never recreates the surface. Square corners (rounded ones
  were offered and declined).
