#ifndef CUSTOMBAR_HYPRLAND_DISPATCH_H
#define CUSTOMBAR_HYPRLAND_DISPATCH_H

// Asks Hyprland to focus workspace `id`. Tries the Lua dispatcher syntax first
// ("dispatch hl.dsp.focus({ workspace = <id> })", no surrounding quotes -- those
// are only needed on the hyprctl *CLI*, where the shell strips them before hyprctl
// sees them; this talks to the socket directly, so literal quote characters would
// reach Hyprland's Lua parser and get rejected). This matches what the reference
// Quickshell bar's own click handler sends, and is what this machine's
// Lua-configured Hyprland (0.56.1) actually accepts -- confirmed live, including
// ruling out hl.dsp.exec_raw() as a config-agnostic alternative (it returns "ok"
// without actually dispatching on this build).
//
// If that's rejected, falls back once to the classic syntax
// ("dispatch workspace <id>") for compatibility with non-Lua configs. Whichever
// form actually worked is cached after the first call (a given Hyprland instance's
// dispatch style doesn't change at runtime), so only the very first click of a
// session could ever pay for a failed probe and its one-time stderr log -- on a
// Lua config (the common case here) that never happens at all, since the Lua form
// is tried first and is expected to succeed immediately.
// Fire-and-forget either way -- Hyprland's response text isn't otherwise acted on
// beyond checking for an error prefix.
void hyprland_dispatch_focus_workspace(int id);

#endif
