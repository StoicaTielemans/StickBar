//! Styling and geometry constants shared across the bar, ported from the C bar's
//! `config/config.c` (itself ported from the reference Quickshell bar's `config.js`).
//!
//! Only values genuinely shared between modules live here. A widget's own one-off
//! tuning (e.g. the workspace pill's padding) stays in that widget's own files,
//! matching the convention the reference project documents.

mod bar;
mod colors;

pub(crate) use bar::BAR;
pub(crate) use colors::COLORS;
