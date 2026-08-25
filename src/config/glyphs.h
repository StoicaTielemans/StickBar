#ifndef CUSTOMBAR_CONFIG_GLYPHS_H
#define CUSTOMBAR_CONFIG_GLYPHS_H

#include <stdint.h>

/*
 * Nerd Font icon codepoints used by bar modules. These live in the Supplementary
 * Private Use Area (5-hex-digit codepoints, e.g. 0xF00EE) where hand-typing or
 * pasting the raw UTF-8 bytes is error-prone -- it's easy to end up with a
 * truncated byte sequence plus a stray literal digit. Every codepoint below was
 * verified present in JetBrainsMonoNerdFont-Regular.ttf's cmap via fontTools before
 * being written here (the volume/mic/device-popup ones were additionally derived
 * by reading the exact codepoint back out of the reference Quickshell bar's own
 * QML source via Python's ord(), not retyped by eye). Kept as numeric codepoints
 * and encoded to UTF-8 at runtime by glyph_utf8() rather than embedding raw
 * multi-byte string literals in source.
 */
#define GLYPH_ICON_CALENDAR 0xF00EEu
#define GLYPH_ICON_CLOCK     0xF0954u

// Volume (sink) level icons, matching VolumeWidget.qml's icon() thresholds
// exactly: muted, <34%, <67%, else.
#define GLYPH_ICON_VOLUME_MUTED  0xF0581u
#define GLYPH_ICON_VOLUME_LOW    0xF057Fu
#define GLYPH_ICON_VOLUME_MEDIUM 0xF0580u
#define GLYPH_ICON_VOLUME_HIGH   0xF057Eu

// Mic (source) icons, matching MicWidget.qml's icon().
#define GLYPH_ICON_MIC_MUTED 0xF036Du
#define GLYPH_ICON_MIC       0xF036Cu

// Device-dropdown row icons, matching AudioDevicesPopup.qml/MicDevicesPopup.qml:
// a "default device" marker vs. a plain selectable-circle marker, and the
// speaker-icon prefix used on sink rows (mic rows reuse GLYPH_ICON_MIC above).
#define GLYPH_ICON_DEVICE_DEFAULT     0xF0967u
#define GLYPH_ICON_DEVICE_SELECTABLE  0xF0130u
#define GLYPH_ICON_HEADPHONES         0xF02CBu

// System monitor icons -- the reference SystemMonitorWidget.qml uses a single
// combined "system monitor" glyph for all three metrics; this project splits
// CPU/GPU/memory into three separate modules (per explicit user request), so
// each gets its own distinct icon instead of sharing one.
#define GLYPH_ICON_CPU 0xF0EE0u // md-cpu_64_bit
#define GLYPH_ICON_GPU 0xF08AEu // md-expansion_card
#define GLYPH_ICON_MEM 0xF035Bu // md-memory

// Media (MPRIS) icons, codepoints derived by reading them directly out of the
// reference MediaWidget.qml/MediaPopup.qml source via Python's ord() (both
// files use the same play/pause pair), not retyped by eye -- confirmed
// present in JetBrainsMonoNerdFont-Regular.ttf's cmap via fontTools, same as
// every other glyph in this file.
#define GLYPH_ICON_MEDIA_PLAY  0xF040Au // md-play
#define GLYPH_ICON_MEDIA_PAUSE 0xF03E4u // md-pause
#define GLYPH_ICON_MEDIA_PREV  0xF04AEu // md-skip_previous
#define GLYPH_ICON_MEDIA_NEXT  0xF04ADu // md-skip_next

// Network icons, matching NetworkWidget.qml's icon() thresholds exactly:
// no default route at all, disconnected (Wi-Fi vs. Ethernet), connected
// (Wi-Fi vs. Ethernet) -- codepoints derived the same way as the media
// icons above (read via Python's ord() from the reference source, not
// retyped by eye).
#define GLYPH_ICON_NETWORK_UNKNOWN 0xF06B0u // md-help_network -- no default route
#define GLYPH_ICON_WIFI_OFF        0xF05AAu // md-wifi_off
#define GLYPH_ICON_ETHERNET_OFF    0xF0202u // md-ethernet_off (actually lan-disconnect)
#define GLYPH_ICON_WIFI_ON         0xF05A9u // md-wifi
#define GLYPH_ICON_ETHERNET_ON     0xF0200u // md-ethernet

// Plain BMP Unicode arrows (not Nerd Font PUA glyphs), used by the network
// module's rx/tx rate label ("↓12.3 KB/s ↑4.5 KB/s", matching
// NetworkWidget.qml's own label text exactly) -- still routed through
// glyph_utf8() rather than hand-typed as raw UTF-8 literals, per this
// project's own convention against hand-typing multi-byte sequences, even
// though these two are common enough that most editors handle them
// correctly.
#define GLYPH_ARROW_DOWN 0x2193u
#define GLYPH_ARROW_UP   0x2191u

// Privacy indicator icons (mic/webcam/screen-share currently in use). Mic
// reuses GLYPH_ICON_MIC above (same glyph the mic/volume modules use).
// Webcam/screen-share codepoints found via a live fontTools cmap search
// against JetBrainsMonoNerdFont-Regular.ttf for glyph names containing
// "webcam"/"monitor_share" -- same verification convention as every other
// glyph in this file, not guessed from a Nerd Font cheat sheet.
#define GLYPH_ICON_WEBCAM      0xF05A0u // nf-md-webcam
#define GLYPH_ICON_SCREENSHARE 0xF1483u // nf-md-monitor_share

// Arch logo + power menu icons, codepoints derived by reading them directly
// out of the reference ArchLogoWidget.qml/PowerMenu.qml source via Python's
// ord() (in PowerMenu.qml's own on-screen order: Logout, Restart,
// Shutdown), not retyped by eye -- confirmed present in
// JetBrainsMonoNerdFont-Regular.ttf's cmap via fontTools, same as every
// other glyph in this file. GLYPH_ICON_ARCH_LOGO is in the font's ordinary
// (BMP) Private Use Area, not the Supplementary PUA every other glyph here
// lives in -- glyph_utf8() already handles both ranges correctly.
#define GLYPH_ICON_ARCH_LOGO 0xF303u  // nf-linux-archlinux
#define GLYPH_ICON_LOGOUT    0xF0343u // nf-md-logout
#define GLYPH_ICON_RESTART   0xF0453u // nf-md-restart
#define GLYPH_ICON_SHUTDOWN  0xF0425u // nf-md-power

// Encodes a single Unicode codepoint as UTF-8 into `out`, nul-terminated.
// `out` must have room for at least 5 bytes (up to 4 bytes of encoding + '\0').
// Returns the number of encoded bytes (excluding the terminator).
int glyph_utf8(uint32_t codepoint, char out[5]);

// Workspace pills render numerals as CJK kanji glyphs instead of digits, matching
// the reference bar. Returns the codepoint for workspace ids 1..10, or 0 for any id
// outside that range (callers should fall back to rendering the plain number).
uint32_t kanji_for_workspace(int id);

#endif
