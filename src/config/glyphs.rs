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
/// `md-volume_off`: the volume box when muted or at 0 %.
pub(crate) const ICON_VOLUME_MUTED: char = '\u{f0581}';
/// `md-volume_low`: under 34 %.
pub(crate) const ICON_VOLUME_LOW: char = '\u{f057f}';
/// `md-volume_medium`: under 67 %.
pub(crate) const ICON_VOLUME_MEDIUM: char = '\u{f0580}';
/// `md-volume_high`: 67 % and up.
pub(crate) const ICON_VOLUME_HIGH: char = '\u{f057e}';
/// `md-microphone`: the mic box.
pub(crate) const ICON_MIC: char = '\u{f036c}';
/// `md-microphone_off`: the mic box when muted or at 0 %.
pub(crate) const ICON_MIC_MUTED: char = '\u{f036d}';
/// `md-headphones`: the output device dropdown's header.
pub(crate) const ICON_HEADPHONES: char = '\u{f02cb}';
/// The network box's "no default route" icon. The C bar calls it
/// `md-help_network`, but the installed font names this code point
/// `md-update`; kept as is for parity with the C bar (see
/// `docs/modules/network.md`).
pub(crate) const ICON_NETWORK_UNKNOWN: char = '\u{f06b0}';
/// `md-wifi_off`: on Wi-Fi, interface down.
pub(crate) const ICON_WIFI_OFF: char = '\u{f05aa}';
/// `md-ethernet_cable_off`: on Ethernet, interface down.
pub(crate) const ICON_ETHERNET_OFF: char = '\u{f0202}';
/// `md-wifi`: on Wi-Fi, up.
pub(crate) const ICON_WIFI: char = '\u{f05a9}';
/// `md-ethernet`: on Ethernet, up.
pub(crate) const ICON_ETHERNET: char = '\u{f0200}';
/// `↓`, before the download rate. Plain Unicode, but a const like every glyph.
pub(crate) const ARROW_DOWN: char = '\u{2193}';
/// `↑`, before the upload rate.
pub(crate) const ARROW_UP: char = '\u{2191}';
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
/// `md-play`: the media box while paused or stopped, the play chip, and the
/// media dropdown's header icon.
pub(crate) const ICON_MEDIA_PLAY: char = '\u{f040a}';
/// `md-pause`: the media box and play chip while playing.
pub(crate) const ICON_MEDIA_PAUSE: char = '\u{f03e4}';
/// `md-skip_previous`: the media dropdown's previous chip.
pub(crate) const ICON_MEDIA_PREV: char = '\u{f04ae}';
/// `md-skip_next`: the media dropdown's next chip.
pub(crate) const ICON_MEDIA_NEXT: char = '\u{f04ad}';
/// `md-checkbox_marked`: a ticked checkbox entry in a tray menu.
pub(crate) const ICON_CHECKBOX_ON: char = '\u{f0132}';
/// `md-checkbox_blank_outline`: an unticked checkbox entry in a tray menu.
pub(crate) const ICON_CHECKBOX_OFF: char = '\u{f0131}';
/// `md-radiobox_marked`: the chosen radio entry in a tray menu.
pub(crate) const ICON_RADIO_ON: char = '\u{f043e}';
/// `md-radiobox_blank`: an unchosen radio entry in a tray menu.
pub(crate) const ICON_RADIO_OFF: char = '\u{f043d}';
/// `md-equalizer`, the EQ dropdown's header icon.
pub(crate) const ICON_EQUALIZER: char = '\u{f0ea2}';

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
