#ifndef CUSTOMBAR_HYPRLAND_DISPATCH_H
#define CUSTOMBAR_HYPRLAND_DISPATCH_H

// Asks Hyprland to focus workspace `id`. Tries the classic dispatcher syntax first
// ("dispatch workspace <id>"); some Hyprland configs (Lua-based ones, at least as of
// 0.56.1) reject that with a syntax error and require
// "dispatch 'hl.dsp.focus({ workspace = <id> })'" instead -- confirmed live against
// a real Lua-configured instance, and matches what the reference Quickshell bar's
// own click handler actually sends. If the first attempt's response looks like an
// error, this retries once with the fallback form. Fire-and-forget either way --
// Hyprland's response text isn't otherwise acted on.
void hyprland_dispatch_focus_workspace(int id);

#endif
