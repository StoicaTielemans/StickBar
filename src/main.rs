//! stickBar: a Wayland status bar for Hyprland, being ported from C to Rust.
//!
//! This is the empty starting point of the port. Nothing is drawn yet; modules
//! land one at a time, each in its own directory under `src/modules/`, matching
//! the layout the C bar used (see CLAUDE.md).

// Belt and braces: Cargo.toml already forbids unsafe code crate-wide, but
// stating it here too means the rule is visible to anyone reading the source,
// and it still holds if the file is ever built outside this Cargo.toml.
#![forbid(unsafe_code)]

use std::process::ExitCode;

/// Entry point. Returning `ExitCode` (instead of calling `process::exit`,
/// which the lint policy blocks) lets every value's destructor run on the way
/// out, so sockets and buffers are always closed cleanly.
fn main() -> ExitCode {
    // stderr, not stdout: the bar's diagnostics have always gone to stderr,
    // and the lint policy blocks println! to keep stdout clean.
    eprintln!("stickBar: Rust port, nothing implemented yet");
    ExitCode::SUCCESS
}
