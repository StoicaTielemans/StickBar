# Shelly smart plugs

A feature with no reference equivalent, added at the user's request
from their own `shelly.py`: switch their Shelly smart plugs, one or all at once, and see
what each one draws. It's a card in the media dropdown, under the EQ card, with
**nothing in the bar itself** (the user's choice).

## What you see

`src/modules/media/shelly_section.rs`, only with a settings file:

- **Header:** plug glyph, `Smart plugs  ·  2/3 on`, and a pill: the total watts of the
  plugs that are on (green), `all off`, `offline` (nothing answers) or `…` (not asked
  yet), grey.
- **One row per plug,** in the file's order:
  - a state dot: green on, grey off, red error, dim offline/unknown;
  - the name;
  - the live watts while it's on (`42 W`, `3.4 W` under 10 W), or `offline`, or the
    error in red;
  - a badge: `ON` green, `off` grey, `…` yellow while a switch is under way, `--` with
    no answer.
- **A click anywhere on a row switches that plug** to the opposite of what the badge
  shows. A row reacts (hand cursor, hover light) only while its state is known and
  nothing is pending: an offline, erroring or not-yet-read plug does nothing.
- **`All on` / `All off` chips** switch every plug, the offline ones included (they
  just answer `offline` again).
- The card's height depends only on the plug count, so nothing ever resizes the
  dropdown.

## Setup

`~/.config/stickBar/shelly.conf` (template: `examples/shelly.conf`), one plug per line, `name = IP` (optionally
`IP:port`), `#` comments allowed. It's read once at start, so restart the bar after
editing it.

```text
desk    = 192.168.1.51
tv      = 192.168.1.52
kitchen = 192.168.1.50
```

- Find the IPs in the Shelly app (device → Settings → Device info) or the router's
  client list, and give each plug a **DHCP reservation**, so it keeps its IP.
- Only IPs, never host names: a DNS lookup can hang far past the plugs' timeouts.
- A bad line, a repeated name, an empty file, or more than 12 plugs turns the card
  off, with the reason on stderr. No file means no card. It holds no secret, so its
  mode doesn't matter.
- **Gen2 and newer only** (the user's plugs are Gen3; Gen4 speaks the same API). A Gen1
  plug shows `not Gen2+`: it has no `/rpc`.
- **No password.** With authentication turned on in the Shelly app, a plug answers 401
  to everything and the row shows `password set`. Gen2+ uses HTTP digest auth, which
  isn't implemented; turn it off for plugs on a trusted LAN.

## How it talks to them (`src/shelly/`)

- **Plain HTTP, by hand:** `http.rs` sends an HTTP/1.0 `GET` over a
  `std::net::TcpStream` and reads to EOF. No HTTP crate, no curl child, no TLS. 1.5 s
  timeouts on connect, read and write.
- **Reading:** `GET /rpc/Switch.GetStatus?id=0`. Its answer carries `output` (on/off)
  and `apower` (W) together, so **the watts cost no extra request**.
- **Switching:** `GET /rpc/Switch.Set?id=0&on=true|false`, then a `GetStatus` for the
  state and watts it ended up with. Always a `Set` to the opposite of what's shown,
  never `Toggle`, so two quick clicks can't flip it twice behind a stale view.
- **Answers:** a refused connect or a timeout means `offline`; 401 `password set`; 404
  `not Gen2+`; anything else (another status, bad JSON, no `output`) an error with a
  short reason, also printed to stderr once when it starts.
- **Threads:** each batch (a poll of every plug, or a switch of one or all) runs on a
  named `shelly` thread, which asks every plug at once from scoped `shelly-req` threads,
  so one unplugged plug doesn't hold the others up. The answers come back together on
  a calloop channel to `App::shelly_result`, which repaints the dropdown.
- **Stale answers are dropped:** each plug counts the switches sent to it, and a
  request carries the count it was sent at. A poll that left before a click and
  answers after it would otherwise flash the old state for 2 s.

## When it asks

- **Only while the media dropdown is open:** once as it opens, then every 2 s (every
  other tick of the 1 s clock timer, which already polls the players' positions while
  it's open), never two polls at once. With the dropdown closed the plugs are never
  asked anything, so it costs nothing when you're not looking.
- **Data:** one poll is about 0.5 KB per plug (an ~80 B request, ~150 B of response
  headers, ~300 B of JSON). Three plugs with the dropdown open come to about
  0.75 KB/s on the LAN; nothing at all with it closed.

## Testing

- `STICKBAR_TEST_SHELLY_CONF`: the settings file, honoured in any mode, and **the only
  one a test bar reads**. `tests/lib/common.py` points every bar at a file that doesn't
  exist (`NO_SHELLY`), so a test can never switch the user's real plugs, and the card
  never changes the dropdown size other tests measure.
- `tests/lib/fake_shelly.py`: fake Gen3 plugs, an HTTP server each on a localhost port,
  logging every request. `dead_addr()` is a port nobody listens on (offline);
  `password=True` answers 401.
- Test-control: `refresh shelly` (a poll now), `dump shelly` (`enabled`, `polling`, and
  per plug `name`, `state`, `watts`, `pending`, `error`), `target shelly <name|all-on|all-off>`,
  and `menu`'s `shelly_rows` (-1 closed, 0 without the card).
- `tests/test_shelly.py` covers it all; `tests/README.md` lists what it checks.
