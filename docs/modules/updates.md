# Arch updates

A Rust-bar-only module (no reference or C-bar equivalent), added at the user's
request: how many package updates are waiting, and which.

## What you see

- **Box** (right region, just left of mic): `<package icon> 54` in pink. Only shown
  while something needs updating, like the privacy box: an up-to-date system (or no
  check yet) costs no bar space. Left click does nothing (user's choice); right click
  opens the dropdown.
- **Dropdown** (`src/modules/updates/popup.rs`, anchored under the box's right edge):
  header `Updates · N packages` and a pill: `checking` yellow, `failed` red, or how long
  ago the last check finished (`just now`, `12 min ago`, `2 h ago`) grey. A red
  `last check: …` line when the last check (partly) failed. Then a `REPO` card and an
  `AUR` card (an empty one left out), one row per package: name left, `old → new` right
  (a long name is shortened, the versions never are). At most 30 package rows in all
  (`MAX_ROWS`): the AUR card keeps up to 10 (`MAX_AUR_ROWS`), the repo card gets the rest,
  and a card cut short ends with `+N more`. A finished check re-lays it out (recreating
  the surface on a height change, the media dropdown's rule); with nothing left it closes.

## Where the numbers come from (`src/updates/mod.rs`)

- **Repo:** `/usr/bin/checkupdates` (pacman-contrib). It syncs a private copy of the
  package databases, so it never takes pacman's lock or changes the real databases.
  Exit 0 = updates listed, **2 = none** (not an error), anything else = failure.
- **AUR:** `/usr/bin/yay -Qua`, skipped if yay isn't installed. Like `pacman -Qu` it
  **exits 1 when there's nothing**, so 1 with no output is "none". It appends an age
  tag (`[1d12h]`) after the versions, which the parser ignores.
- **Parsing:** `name old -> new`, whitespace-separated; any other line (warnings) is
  skipped.
- **Threading:** both commands run on a named `updates-check` thread (they need the
  network and can take seconds), one check at a time. The result comes back on a
  calloop channel to `App::updates_result`, which marks the bars dirty and refreshes an
  open dropdown. A source that fails keeps its last good list; the error is shown.
- **Child processes** are started with `util::spawn` (`posix_spawn`, empty signal mask,
  SIGPIPE default, stdin/stderr on `/dev/null`, stdout on a close-on-exec pipe), the
  same helper the EQ's profile script uses. Rule (from the EQ): a child of the bar must
  never inherit its blocked SIGINT/SIGTERM/SIGUSR1. `test_updates` checks `SigBlk` is 0.

## When it checks

- **60 s after start** (the network is up by then at login), then **every 30 min**.
- **10 s after the package database changes:** inotify on `/var/lib/pacman/local`
  (`CREATE | DELETE | MOVED_TO`: every install, removal or upgrade replaces a package's
  directory). The first event schedules one re-check after a 10 s settle (an upgrade
  touches hundreds of entries); events meanwhile don't schedule more. So the count drops
  right after running `yay` yourself.
- **Never automatically in test mode** (`STICKBAR_TEST_CONTROL=1`): test bars check only
  on the tests' `refresh updates`, so a test bar never runs pacman or touches the network
  unasked, and `test_no_subprocess` stays clean.

## Testing

- `STICKBAR_TEST_UPDATES_REPO` / `STICKBAR_TEST_UPDATES_AUR` (absolute paths) replace
  `checkupdates` / `yay`. `tests/lib/fake_updates.py` plays both.
- Test-control: `refresh updates` (start a check), `dump updates` (`total`, `repo`/`aur` as
  `name old -> new` lines, `checking`, `last_error`), and `menu`'s `update_rows` (package
  rows shown while the dropdown is open).
- Checked once against the real system (2026-11-13): 53 repo + 1 AUR, identical to
  running `checkupdates` and `yay -Qua` by hand; the check took 1.5 s.
