# Clock year-calendar popup

The reference bar's `ClockWidget.qml` shows a small current-week strip on
hover and a full 12-month year-calendar grid (`ClockYearPopup.qml`) on
left-click, toggled via `yearOpen = !yearOpen`. This project reproduces only the
year grid (`ClockWeekPopup.qml`'s week strip needs hover, see [popups.md](../popups.md)), toggled the
same way but on **right**-click.

- **The year/month/day grid is computed from scratch in C** (no equivalent of
  `CalendarUtils.js` existed yet): `days_in_month()`/`first_weekday_mon0()`
  both go through `mktime()`'s own date normalization (asking for "day 0 of
  the following month" rather than a hand-rolled leap-year table, and reading
  back `tm_wday` for the 1st of the month) rather than reimplementing calendar
  math by hand — noon (`tm_hour = 12`) is used for both to stay clear of any
  DST transition exactly at midnight.
- **Every month card is padded to a uniform 6 week-rows**, unlike the
  reference's own per-month-height `Repeater` (which lets Qt's `GridLayout`
  leave blank space under a shorter card in the same row when neighboring
  cards in that row need more weeks) — a deliberate simplification so all 12
  cards line up into a perfectly uniform grid via plain arithmetic, without
  needing to replicate Qt's row-height-negotiation behavior.
- **Look**: still the 4×3 year grid, with:
  - **Header**: `‹`/`›` chevrons (`GLYPH_CHEVRON_LEFT/RIGHT`, nf-fa
    0xF053/0xF054, confirmed in the font's cmap) flip the shown year; between
    them the year (mauve, `FONT.title`) and `· Tuesday, 3 November` (today), or
    `· back to 2026` when another year is shown — clicking that text jumps back
    to the current year. The three targets get a `surface0` hover background.
    Year navigation made this the first calendar click target: `pointer.c` now
    routes menu-surface clicks to `clock_calendar_popup_handle_click()` and
    motion to `_handle_motion()` instead of swallowing them, and clears the
    hover on `pointer_leave`. The popup always opens on the current year.
  - **Month cards**: each month sits on a `base` card with a 1px `surface1`
    border; the current month's card (current year only) gets a 2px mauve
    border and a mauve name.
  - **ISO week numbers** in a faint left column (`overlay0`, `FONT.xs`), only on
    rows containing a day of that month. Computed by `iso_week_of()` on each
    row's *Thursday* via `mktime` + `strftime("%V")` — the Thursday decides a
    week's ISO year, and `mktime` normalizes an out-of-month day into the
    neighbouring month/year, so straddling rows are right (1 Jan 2027, a
    Friday, shows week 53). Checked against `date +%V`.
  - **Readability**: day numbers went from `FONT.xs` (8px) in 18px cells to
    `FONT.sm` (10px) in 22px cells. Weekends (and the S/S initials) are `red`;
    days before today are drawn at 40% alpha (every day of a past year, none of
    a future one); today is still a filled mauve circle with bold text.
  - Size grew from ~700×600 to ~880×680 px, still right-anchored under the
    clock. It's identical for every year (uniform 6-row cards), so flipping
    years only re-renders the existing surface, never recreates it.
  - No rounded corners — offered and declined; the popup keeps the bar's
    square mantle-with-mauve-border look.
- **Three new shared font-size tokens were added to `config/config.h`'s
  `FontConfig`** (`xs`=8, `md`=11, `title`=16), ported directly from the
  reference `config.js`'s own `font.sizes.{xs,md,title}` values — these are
  generic enough (fine print, secondary emphasis, large titles) to be genuine
  shared tokens rather than one-off local tuning, unlike e.g. the workspace
  pill's own local padding constants.

## In the Rust port

- **Files:** `src/modules/clock/mod.rs` holds `Clock`, which formats and caches the text and
  draws the box. `src/modules/clock/timer.rs` holds the tick. The box goes through
  `src/render/module_box.rs` and the text through `src/render/text.rs`.
- **The tick is a `CLOCK_REALTIME` timerfd armed `ABSTIME | CANCEL_ON_SET`** at the next
  whole second, the same as the C bar. It uses rustix's safe timerfd wrappers, because calloop's
  `Timer` is monotonic-only. On `ECANCELED` (the wall clock was set) it re-arms on the new second.
- **Clock-only redraw:** a tick that changes the text marks bars `Dirty::Clock`. The bar copies
  its previous frame and repaints just the clock's box. If the clock's width changed, it falls
  back to a full redraw (`bar/bar_surface.rs`, `render::Renderer::render_clock_only`).
- The box is pixel-identical to the C bar's (checked with `grim` on both, byte-equal PNGs).
