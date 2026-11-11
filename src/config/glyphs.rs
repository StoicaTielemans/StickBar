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
/// `fa-chevron_left`, the calendar's previous-year button.
pub(crate) const ICON_CHEVRON_LEFT: char = '\u{f053}';
/// `fa-chevron_right`, the calendar's next-year button.
pub(crate) const ICON_CHEVRON_RIGHT: char = '\u{f054}';
/// `md-cpu_64_bit`, the CPU box and its dropdown's header.
pub(crate) const ICON_CPU: char = '\u{f0ee0}';
/// `md-expansion_card`, the GPU box and its dropdown's header.
pub(crate) const ICON_GPU: char = '\u{f08ae}';
/// `md-memory`, the memory box and its dropdown's header.
pub(crate) const ICON_MEM: char = '\u{f035b}';
/// `linux-archlinux`, the bar's leftmost item (opens the power menu). In the
/// font's ordinary (BMP) Private Use Area, unlike the 5-digit `md-` icons.
pub(crate) const ICON_ARCH: char = '\u{f303}';
/// `md-logout`, the power menu's Logout card.
pub(crate) const ICON_LOGOUT: char = '\u{f0343}';
/// `md-reload` (the C bar calls it "restart"), the power menu's Restart card.
pub(crate) const ICON_RESTART: char = '\u{f0453}';
/// `md-power`, the power menu's Shutdown card and its header icon.
pub(crate) const ICON_POWER: char = '\u{f0425}';

/// Workspace numerals 1-10 as kanji (一 二 三 四 五 六 七 八 九 十), matching the
/// reference bar. These are CJK characters, not Nerd Font icons, so pango
/// draws them from a CJK fallback font (Noto Sans CJK here). Each was read out
/// of the reference Quickshell config's `kanjiMap` with Python's `ord()`
/// rather than retyped (the CLAUDE.md rule for kanji).
const KANJI: [char; 10] = [
    '\u{4e00}', '\u{4e8c}', '\u{4e09}', '\u{56db}', '\u{4e94}', '\u{516d}', '\u{4e03}', '\u{516b}',
    '\u{4e5d}', '\u{5341}',
];

/// The kanji numeral for workspace `id`, or `None` outside 1-10 (the caller
/// then shows the plain number).
pub(crate) fn kanji(id: i32) -> Option<char> {
    usize::try_from(id)
        .ok()
        .and_then(|id| id.checked_sub(1))
        .and_then(|i| KANJI.get(i))
        .copied()
}

#[cfg(test)]
mod tests {
    use super::kanji;

    #[test]
    fn kanji_numerals() {
        assert_eq!(kanji(1), Some('\u{4e00}'));
        assert_eq!(kanji(10), Some('\u{5341}'));
        assert_eq!(kanji(0), None);
        assert_eq!(kanji(11), None);
        assert_eq!(kanji(-3), None);
    }
}
