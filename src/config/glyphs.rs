//! Nerd Font icon code points (the C bar's `config/glyphs.h`).
//!
//! Icons live in the Private Use Area, where pasting the raw character into
//! source is error-prone (in C, an easy way to end up with a truncated UTF-8
//! sequence). So every icon is a named `char` written as a `\u{...}` escape
//! (`non_ascii_literal` makes a pasted one a build error), and each code point
//! was checked against the installed font's cmap with fontTools before being
//! written here:
//!
//! ```text
//! python3 -I -c "from fontTools.ttLib import TTFont; \
//!   print(0xF00EE in TTFont('/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Bold.ttf').getBestCmap())"
//! ```
//!
//! Unlike C, no runtime UTF-8 encoder is needed: a Rust `char` is always a
//! valid code point, and pushing it onto a `String` encodes it correctly.

/// `md-calendar_blank`, before the clock's date.
pub(crate) const ICON_CALENDAR: char = '\u{f00ee}';
/// `md-clock`, before the clock's time.
pub(crate) const ICON_CLOCK: char = '\u{f0954}';
