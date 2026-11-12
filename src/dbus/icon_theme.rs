//! The PNG fallback for a tray item without a usable `IconPixmap` (the C bar's
//! `dbus/icon_theme.c`): its `IconName` looked up as a PNG file. Not the real
//! freedesktop icon-theme spec (no `index.theme`, no inheritance, no SVG; see
//! `docs/limitations.md`), just the places that work in practice. This side
//! only finds and reads the file; the main thread decodes it with cairo.

use std::{
    fs,
    path::{Path, PathBuf},
};

/// hicolor size directories to try, largest reasonable first: shrinking a
/// bigger icon looks better than growing a tiny one.
const HICOLOR_SIZES: [&str; 5] = ["48x48", "64x64", "32x32", "24x24", "16x16"];

/// The 8 bytes every PNG file starts with. A file that doesn't is skipped,
/// as C skipped one cairo couldn't load.
const PNG_SIGNATURE: [u8; 8] = [0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1a, b'\n'];

/// The bytes of the first PNG found for `icon_name`: the item's own
/// `IconThemePath` (Steam points it at its install), then the user's and the
/// system's hicolor themes, then `/usr/share/pixmaps`.
pub(crate) fn lookup(icon_name: &str, theme_path: &str) -> Option<Vec<u8>> {
    // A name with a path separator would escape the directories below.
    if icon_name.is_empty() || icon_name.contains('/') {
        return None;
    }
    let file = format!("{icon_name}.png");
    let mut candidates = Vec::new();
    if !theme_path.is_empty() {
        candidates.push(Path::new(theme_path).join(&file));
    }
    let user_data = std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME")
                .filter(|v| !v.is_empty())
                .map(|home| Path::new(&home).join(".local/share"))
        });
    let mut hicolor_bases: Vec<PathBuf> = user_data
        .map(|d| d.join("icons/hicolor"))
        .into_iter()
        .collect();
    hicolor_bases.push(PathBuf::from("/usr/share/icons/hicolor"));
    hicolor_bases.push(PathBuf::from("/usr/local/share/icons/hicolor"));
    for base in &hicolor_bases {
        for size in HICOLOR_SIZES {
            candidates.push(base.join(size).join("apps").join(&file));
        }
    }
    candidates.push(Path::new("/usr/share/pixmaps").join(&file));
    candidates
        .iter()
        .filter_map(|path| fs::read(path).ok())
        .find(|bytes| bytes.starts_with(&PNG_SIGNATURE))
}
