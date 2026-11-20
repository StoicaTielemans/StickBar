# Proxmox

A module with no reference equivalent, added at the user's
request: the state of their Proxmox VE server, a way to wake it with Wake-on-LAN, and
a way to shut it down.

## What you see

- **Box** (left region, right after the memory box, before the privacy indicator). Only shown with a settings
  file. `<md-server icon>` plus:
  - `3/5` in green: the API answers, and 3 of the 5 VMs/containers run (templates
    aren't counted, paused guests are);
  - `off` in grey: nothing answers;
  - `…` in yellow: waking (a magic packet was sent, and the API hasn't answered yet);
  - `3/5` in peach: shutting down (Proxmox accepted the shutdown and is stopping
    guests). The count falls as they stop; `…` before any count is known;
  - `!` in red: it answers, but with an error (a refused token, a pin mismatch…);
  - `--` in grey: no poll has finished yet.

  Right click opens the dropdown. **Left click does nothing**, so a stray click never
  boots the server (the user's choice): waking and shutting down are only the
  dropdown's chips.
- **Dropdown** (`src/modules/proxmox/popup.rs`, anchored under the box's left edge,
  growing rightwards like the CPU/GPU/memory dropdowns).
  The frame takes the box's colour.
  - **Header:** `Proxmox · pve` (the node names, or the host before the first answer)
    and a pill: `up 3d 4h` green, `off` grey, `waking` yellow, `shutting down` peach,
    `error` red, `…` grey.
  - **Off, waking or no answer yet:** a note (`no answer from 192.168.0.50: it's off`,
    or `magic packet sent 40 s ago…`) and the **Wake** chip, green, or yellow `Send
    again` while waking. It's the dropdown's only clickable thing (hand cursor).
  - **Error:** a red line with the reason, above the last good figures. The same line
    shows why a Wake or Shut down failed (the box doesn't change for that); it clears
    on the next successful action or when the dropdown reopens.
  - **Shutting down:** a note (`shutdown sent 12 s ago, stopping guests…`) over the
    last figures, and no chip. Meanwhile Proxmox lists the node with every figure 0
    (its status daemon has stopped); a node card shows `--` and `no figures right
    now` for that, instead of `0% of 0 cores`.
  - **Up:**
    - one card per node: CPU, RAM and root-disk meters (the CPU dropdown's green →
      red gradient) with `12% of 8 cores` / `8.0 GiB / 32.0 GiB`, and its uptime;
    - a `GUESTS` card, one row per guest: a state dot (green running, yellow paused,
      grey stopped), `VM`/`CT`, the id, the name, and CPU % with RAM used / allowed
      while running (`stopped`/`paused` otherwise). At most 20 rows, then `+N more`;
    - a `STORAGE` card: name, used / total and %, or `unavailable` in red;
    - at the very bottom, the **Shut down** chip (see below).
  - It re-lays out after every poll; a height change resizes it
    ([popups.md](../popups.md)).

## Setup

1. **A read-only API token.** On the server (shell, or Datacenter → Permissions → API
   Tokens in the web UI):

   ```sh
   pveum user token add root@pam stickbar --privsep 1   # prints the secret once
   pveum acl modify / --tokens 'root@pam!stickbar' --roles PVEAuditor
   ```

   `PVEAuditor` can read everything and change nothing, so the token on this PC can't
   start, stop or delete anything. For the Shut down chip, also give it the right to
   power the node off, and nothing else:

   ```sh
   pveum role add StickbarPower --privs Sys.PowerMgmt
   pveum acl modify /nodes/pve --tokens 'root@pam!stickbar' --roles StickbarPower
   ```

   It still can't touch guests, storage or settings. Without this, Shut down answers
   403 and the dropdown says "the token lacks Sys.PowerMgmt". With privilege separation the token needs its own
   ACL entry; without it Proxmox answers 200 with an empty list, which the bar reports
   as "the token sees no node".
2. **The pin**, the hash of the server's public key, as curl's `--pinnedpubkey` wants
   it:

   ```sh
   echo | openssl s_client -connect 192.168.0.50:8006 2>/dev/null \
     | openssl x509 -pubkey -noout | openssl pkey -pubin -outform der \
     | openssl dgst -sha256 -binary | base64
   ```
3. **`~/.config/stickBar/proxmox.conf`** (template: `examples/proxmox.conf`), mode 600 (the bar warns on stderr if others
   can read it). It's read once at start, so restart the bar after editing it.

   ```text
   url   = https://192.168.0.50:8006
   token = root@pam!stickbar=<secret>
   pin   = sha256//<the base64 from step 2>
   mac   = 7c:05:07:b0:c4:c0
   ```

   An unknown key, a missing `url`/`token`/`mac`, a bad MAC or a non-https URL turns
   the module off with the reason on stderr.
4. **Wake-on-LAN must be on in the server's BIOS and NIC** (`ethtool <nic>` shows
   `Wake-on: g`). Give the server a DHCP reservation too: its certificate was made for
   .148 and it now sits at .50. The pin doesn't care, but `url` does.

## Where the data comes from (`src/proxmox/`)

- **One call per poll:** `GET /api2/json/cluster/resources` returns nodes, guests
  and storage with their live figures (`resources.rs`). `cpu` is a fraction of
  `maxcpu`, sizes are bytes, templates have `template: 1`.
- **curl, not a Rust HTTP crate:** `/usr/bin/curl` through `util::spawn` on a named
  `proxmox-poll` thread, one poll at a time, `--connect-timeout 2 --max-time 5`. A
  TLS stack in the bar would cost more than a curl run every 30 s. The result reaches
  `App::proxmox_result` on a calloop channel.
- **The token never shows in a process list.** Arguments are readable by every user
  in `/proc/PID/cmdline`, so the header goes to curl as a config line on stdin
  (`--config -`, `util::spawn::run_capture_with_stdin`: a pipe filled before the
  child starts, holding only those bytes and never the bar's own stdin, which is
  the test channel in test mode). `config.rs` only accepts the characters a
  Proxmox token is made of, so nothing needs escaping in that line.
- **TLS:** the certificate is self-signed and names the server's old IP. With `pin`,
  curl skips the certificate checks (`--insecure`) but refuses any server whose
  public key doesn't hash to the pin. That works at any IP, and is stricter than
  trusting a CA. Without `pin`, curl verifies the certificate the normal way.
- **Off vs. broken:** curl exit 7 (couldn't connect) or 28 (timeout) means off. Exit
  6, 35, 60 or 90, any other exit, HTTP 401/403/other, bad JSON, or no node in the
  answer means error, and the dropdown says which. An error keeps the last figures;
  off drops them.

## When it polls

- 2 s after start, then every 30 s: a 3 s timer tick polls when one is due.
- **Every tick (3 s) while the dropdown is open**, plus once immediately on opening.
- **Every tick while waking.** Waking ends when the API answers (any answer, even an
  error) or after 5 minutes without one; an "off" poll meanwhile keeps it waking.
- **Never automatically in test mode:** test bars poll only on `refresh proxmox`.

## Wake-on-LAN (`src/proxmox/wol.rs`)

The magic packet, 6×`0xFF` followed by the MAC 16 times (102 bytes), sent from the
bar itself as a UDP broadcast to `255.255.255.255:9` (`SO_BROADCAST`). It's what
`wakeonlan <mac>` sends, without the package or a subprocess, and it's one
non-blocking `sendto` on the main thread. Whether it worked shows only when the server
answers.

## Shut down

- **Two clicks, by design:** shutting the node down stops every VM and container on
  it, so one stray click must never do it. The first click **arms** the chip (filled
  red, `Click again to shut down`). A second click within 5 s sends it. A click after
  that re-arms instead. An armed chip disarms on the 3 s tick once the 5 s are up, and
  when the dropdown closes. The arming lives in the popup (`ProxmoxPopup::press`),
  not the data.
- **The request:** `POST /api2/json/nodes/{node}/status` with `command=shutdown`,
  through the same curl setup as the poll (token on stdin, pin, timeouts), on a named
  `proxmox-power` thread. It comes back as `Msg::Shutdown` on the poll's channel.
  `{node}` is the first node of the last answer, checked to be letters, digits and
  `-` since it goes in the URL. Proxmox answers at once: the shutdown itself runs as
  a task there, stopping guests in their shutdown order, then powering off.
- **Shutting down** starts when the request is accepted (200). Polls run every tick
  until nothing answers (→ `off`, Wake chip back), or for at most 5 minutes (a guest
  that won't stop makes Proxmox give up). Answers meanwhile, even errors, don't end
  it: the API keeps answering while guests stop.
- **A test bar never shuts a real server down:** in test mode the request is only
  sent when `STICKBAR_TEST_PROXMOX_CURL` names a fake (the power menu's rule,
  `docs/modules/power-menu.md`). Don't remove that guard.

## Testing

- `STICKBAR_TEST_PROXMOX_CONF` (settings file, honoured in any mode; **the only one a
  test bar reads**). `tests/lib/common.py` points every bar it starts, in test mode or
  not, at a file that doesn't exist (`NO_PROXMOX`), so the user's real file never adds
  a box to other tests' bars or polls their server (`test_no_subprocess` would catch
  the curl child),
  `STICKBAR_TEST_PROXMOX_CURL` (replaces curl; `tests/lib/fake_proxmox.py`) and
  `STICKBAR_TEST_PROXMOX_WOL_ADDR` (where the magic packet goes; in test mode without
  it nothing is sent, so a test bar never broadcasts on the LAN).
- Test-control: `refresh proxmox`, and `dump proxmox`, which gives `enabled`, `phase`,
  `label`, `nodes`, `storages`, `guests` as `vmid name state` lines, `polling`,
  `last_error` and `action_error`. `menu` adds `proxmox_rows` (guest rows shown) and,
  while a chip shows, `proxmox_chip` `{x, y, kind, armed}` (its middle, for
  `menu-click`; `kind` is `wake` or `shutdown`).
- `tests/test_proxmox.py` covers the states, the dropdown, the packet's exact bytes,
  the two-click shutdown (arming, the 5 s window, 403, 200 → shutting down → off),
  the token staying out of argv, and `SigBlk` 0.
