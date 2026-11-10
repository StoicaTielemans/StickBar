# EQ (TRN Black Pearl)

A StickBar-only module (no reference-bar equivalent) showing the hardware EQ
of the user's TRN Black Pearl USB DAC, and switching between EQ profiles.
The DAC itself is driven entirely by the separate DacController project's
`blackpearl-eq.py` (`~/Code/github.com/StoicaTielemans/DacController`) --
this module never opens the DAC's HID device.

- **Why a state file instead of talking to the DAC.** The script owns the
  DAC's WalkPlay HID protocol. A second process reading the DAC while the
  script writes it steals replies on the shared hidraw node and makes the
  script's read-back verification fail (seen directly while developing the
  script, with the vendor website open at the same time). So the script
  writes `$XDG_CACHE_HOME/blackpearl-eq/state.json` (default
  `~/.cache/blackpearl-eq/`) after every command that changes or reads the
  EQ -- profile name, gain mode, preamp, enabled flag, the bands, and a
  pre-computed 64-point dB curve (20 Hz..20 kHz, log-spaced, preamp *not*
  included) plus `peak_db`/`min_db` -- and the bar only ever reads that file.
  No biquad math lives in C; the script computes the curve from the same RBJ
  coefficients it sends to the DAC.
- **Live updates: inotify, not polling.** `eq/eq_state.c` watches the
  *directory* (not the file -- the script replaces it via temp file +
  `rename()`, which would orphan a watch on the old inode) for
  `IN_MOVED_TO | IN_CLOSE_WRITE`, filters events to the name `state.json`
  (the script's own `state.json.tmp` also shows up), and reloads. The
  directory is created at startup if missing so the watch can be armed
  before the script has ever run. Registered on `main.c`'s epoll loop like
  every other fd; redraw only when `eq_state_pump()` reports an actual parse.
- **Applying a profile.** Clicking a dropdown card runs
  `/usr/bin/python3 <script> profile NAME` via a double fork (the
  intermediate child is reaped immediately, so the script's process is
  reparented to init and never becomes a zombie, without a global `SIGCHLD`
  handler). Everything the child needs (argv, an empty sigset) is prepared
  before `fork()`, since the bar is multi-threaded (pulse/pipewire threads)
  and only async-signal-safe calls are allowed between fork and exec --
  which is also why it's `execv()` with an absolute interpreter path rather
  than `execvp()`. The child resets its signal mask (main.c blocks SIGUSR1
  for the power menu's signalfd, and a blocked mask survives exec) and
  sends stdout/stderr to `/dev/null`. The script sets gain mode and desktop
  volume itself (per the preset's `Gain:`/`Volume:` lines), writes the EQ,
  and writes the state file, which arrives back via inotify. The script
  path can be overridden with `BLACKPEARL_EQ_SCRIPT` (absolute path).
- **Pending state.** The applied name shows as "NAME..." in the bar box, and
  as "applying..." on its dropdown card plus a yellow `applying` header pill,
  until the next state write, or for at
  most 15 s (`PENDING_TIMEOUT_SEC`) -- the script only fails without writing
  state when the DAC is unplugged or inaccessible, and reports that on its
  own stderr, which the bar discards.
- **Rendering.** `eq/eq_curve.c` is pure cairo (same plumbing/rendering split
  as `dbus/sni_icon.c`); both the 34px bar sparkline and the dropdown's
  300x110 graph use it. The vertical scale is the smallest multiple of a step that
  fits the curve's own peak/min, with a floor (+/-6 dB in the bar, +/-12 dB in
  the dropdown), so gentle EQs aren't exaggerated and heavy ones aren't
  clipped. A disabled EQ (`blackpearl-eq.py off`) is drawn in `overlay0`
  instead of the module's `lavender` accent.
- **Offered profiles** are a fixed list in `modules/eq/eq_popup.c`
  (`zero2`, `dt990`, `dt990-loud`, `flat-high`, `flat-low`, per explicit user
  request -- a subset of
  DacController's `presets/`); each must match a `presets/NAME.txt`. Right-click
  opens the dropdown (left-click does nothing, per user request), which stays open after a click, like
  `audio/device_popup.c`, and redraws when the new state arrives.
- **Dropdown contents** (fixed size, so a state change only repaints):
  - **Header**: equalizer glyph, profile name, `· low gain`, and a pill:
    `on` green, `off` or `no data` grey, `applying` yellow.
  - **Graph card**: `preamp -3.0 dB` and the curve's peak/min on a label row
    (the preamp used to be in the header), then the dB/frequency grid and
    curve.
  - **Profile cards**: one per profile under a `PROFILES` label, like the
    audio device cards: accent border + name on the active one (tag `active`),
    `surface0` fill on hover (tag `apply`), yellow `applying...` tag while
    pending. The description stays visible on the right throughout.
- **Testing without touching the DAC.** Run a second bar instance with
  `XDG_CACHE_HOME` pointed at a scratch directory and write a state file
  there (e.g. by importing `blackpearl-eq.py` and calling its `save_state()`)
  -- this is how the inotify path was verified. `tests/test_eq.py` does the
  same and also sets `BLACKPEARL_EQ_SCRIPT` to `tests/lib/fake_eq_script.py`,
  so a dropdown click runs the fake instead. A click has never been verified
  end to end against the real script and DAC.
