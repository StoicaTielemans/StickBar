# EQ (TRN Black Pearl)

Files: `src/eq/mod.rs` (`EqState`: the snapshot, pending/running, the inotify watch),
`src/eq/launcher.rs` (`EqState::apply`), `src/eq/curve.rs` (the curve, pure cairo), and
`src/modules/media/eq_section.rs` (`PROFILES`, `EqSection`: the card, its chips and
hit-testing). `EqState` is `Media::eq`.

No reference equivalent: it shows the hardware EQ of the user's TRN Black Pearl USB DAC and
switches profiles. The DAC is driven entirely by the separate DacController project's
`blackpearl-eq.py` (`~/Code/github.com/StoicaTielemans/DacController`); the bar never opens
the DAC's HID device. **It's a card in the media dropdown only** (the user's choice, [media.md](media.md)):
nothing in the bar itself (a sparkline at the end of the media box was tried and removed:
"keep the song title"). The media box never goes away, so the card is always one right-click
away.

## The state file

- **Why a state file instead of talking to the DAC:** the script owns the DAC's WalkPlay HID
  protocol, and a second process reading the DAC while the script writes it steals replies on
  the shared hidraw node and breaks the script's read-back check. So the script writes
  `$XDG_CACHE_HOME/blackpearl-eq/state.json` (default `~/.cache/blackpearl-eq/`) after every
  command that changes or reads the EQ: profile name, gain mode, preamp, enabled flag, the
  bands, and a pre-computed 64-point dB curve (20 Hz..20 kHz, log-spaced, preamp not
  included) with `peak_db`/`min_db`. The bar only reads that file; no filter maths here.
- **Live updates: inotify on the directory** (rustix's `fs::inotify`, a calloop source), not
  on the file: the script replaces it with a temp file + `rename()`, which would orphan a
  watch on the old inode. `IN_MOVED_TO | IN_CLOSE_WRITE`, filtered to the name `state.json`
  (the script's `state.json.tmp` shows up too). The directory is created at startup so the
  watch works before the script has ever run. `App::eq_file_changed` repaints only when the
  file parsed; the bars are never redrawn for the EQ.

## Applying a profile

- **A click on a profile chip** runs `/usr/bin/python3 <script> profile NAME`. The script sets
  gain mode and desktop volume itself (per the preset's `Gain:`/`Volume:` lines), writes the
  EQ and the state file, which comes back through inotify. `BLACKPEARL_EQ_SCRIPT` (an
  absolute path) overrides the script.
- **Launching:** an `eq-script` thread starts the script through `util::spawn`
  ([architecture.md](../architecture.md), "Child processes") and `waitpid`s it, which reaps
  it, then reports the exit on a calloop channel. `App::eq_script_exited` clears pending and
  reads the state again, in case the exit was handled before the inotify event.
- **"applying" follows the script process, not a timeout.** It starts on the click and ends
  on the state write or the script's exit, whichever comes first. A script that fails without
  writing state (DAC unplugged; it reports on its own stderr, which goes nowhere) clears it
  the moment it exits. A script that hangs keeps showing "applying", which is true. **While
  one runs, profile clicks do nothing** (two scripts at once steal each other's DAC replies):
  no hover, no `apply` tag, arrow cursor.
- Switching profiles on the real DAC has been checked by hand.

## The card

- **Profiles** are a fixed list (`PROFILES`: `zero2`, `dt990`, `dt990-loud`, `flat-high`,
  `flat-low`, the user's pick from DacController's `presets/`); each must match a
  `presets/NAME.txt`. What each one is for is in `PROFILES`' doc comment.
- **Top to bottom**: a header row (equalizer glyph, profile name, `· low gain`, and a pill:
  `on` green, `off` or `no data` grey, `applying` yellow), the preamp and the curve's
  peak/min, the curve on its dB/Hz grid (90 px tall), and one compact chip per profile.
  Chips: active = lavender border and name, applying = yellow, hover = `surface0`.
- **The curve** (`curve.rs`): the vertical scale is the smallest multiple of a step that fits
  the curve's own peak/min, at least ±12 dB, so gentle EQs aren't exaggerated and heavy ones
  aren't clipped. A disabled EQ (`blackpearl-eq.py off`) is drawn in `overlay0` instead of
  `lavender`.
- **Its size doesn't depend on the state**, so a state change only repaints the dropdown
  (`App::eq_state_changed`).
- Test-control: `dump eq`, `target eq NAME` (a chip's middle), `target eq NAME:edge` (its
  left border), `target eq pill`, and `menu`'s `eq_cards` (the chip count).

## Testing without the DAC

**Never point a test at the real script.** `tests/test_eq.py` sets `XDG_CACHE_HOME` to a
scratch directory, writes state files there, and sets `BLACKPEARL_EQ_SCRIPT` to
`tests/lib/fake_eq_script.py`, so a chip click runs the fake. It also checks the fake's
`SigBlk` is 0.
