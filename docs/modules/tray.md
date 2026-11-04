# D-Bus / system tray

The tray talks two separate freedesktop-ecosystem protocols over the D-Bus
session bus via `sd-bus` (`libsystemd`, chosen over `dbus-1`/libdbus for a
leaner, more modern C API — confirmed present via `pkg-config --modversion
libsystemd`): StatusNotifierItem (SNI, the tray-icon protocol) and DBusMenu (the
right-click menu protocol). Both were reverse-verified against this machine's
*real* running tray items (`busctl --user ...` against a live EasyEffects and
Steam) rather than only against spec text — several details below only became
clear that way.

- **sd-bus is driven from the same external epoll loop as everything else**
  (`dbus/dbus_loop.c`), using the same shape as the Wayland/Hyprland fds already
  in `main.c`: `sd_bus_get_fd()` for the fd, `sd_bus_process()` looped until it
  returns 0 to drain available work, then `sd_bus_get_events()` /
  `sd_bus_get_timeout()` re-applied *every* pump (not just once at connect) since
  sd-bus's interest mask and internal deadline both change over time (e.g. wants
  `EPOLLOUT` mid-call). The timeout is a genuinely separate timerfd, converted
  from sd-bus's absolute `CLOCK_MONOTONIC` microsecond deadline to a relative
  `itimerspec` — this is what makes sd-bus's own call-timeout bookkeeping fire at
  all when it isn't running its own default event loop.
- **stickBar can act as either the StatusNotifierWatcher itself or a Host
  client of someone else's, decided automatically at startup, with no other
  code needing to know or care which mode is active.** `main.c` always tries
  `dbus/sni_watcher_service.c`'s `sni_watcher_service_try_start()` first — it
  attempts `sd_bus_request_name(bus, "org.kde.StatusNotifierWatcher", 0)`, and
  if that succeeds, registers the `/StatusNotifierWatcher` object itself
  (`RegisterStatusNotifierItem`/`RegisterStatusNotifierHost`,
  `RegisteredStatusNotifierItems`/`IsStatusNotifierHostRegistered`/
  `ProtocolVersion`, and the `StatusNotifierItem{,Un}Registered`/
  `StatusNotifierHostRegistered` signals) so items register directly with this
  process. Only if that name is already owned by something else (confirmed
  live by running with Quickshell stopped entirely: stickBar claimed
  `org.kde.StatusNotifierWatcher` itself, and both EasyEffects and Steam showed
  up in `RegisteredStatusNotifierItems` with no other Watcher-providing process
  running) does `main.c` fall back to the original, unchanged Host-only path in
  `dbus/sni_watcher.c`'s `sni_watcher_register_host()` — connecting as a client
  to whichever Watcher does exist instead. `dbus/sni_registry.c`,
  `modules/tray/tray.c`, etc. are identical in both modes; they only ever see
  `sni_registry_add()`/`_remove()` calls, regardless of which path triggered
  them. `sni_watcher_service_try_start()` also best-effort claims
  `org.freedesktop.StatusNotifierWatcher` as a second name (failure ignored)
  for any host hardcoded to look for that name instead, reusing the same
  vtable/object registration.
  - **Item registration accepts both real-world calling conventions**, the same
    two `sni_watcher.c`'s client-side `split_bus_name_path()` already parses:
    if the `RegisterStatusNotifierItem(service)` argument starts with `/`, it's
    an object path and the bus name is the caller's own unique name (via
    `sd_bus_message_get_sender()`) — what Steam does; otherwise `service` is
    itself the bus name and the path defaults to `/StatusNotifierItem` — the
    KDE convention EasyEffects uses.
  - **Auto-unregister on item process exit** subscribes to
    `org.freedesktop.DBus`'s `NameOwnerChanged(sss)` signal (this fires for
    *every* bus-name ownership change system-wide, not just ones stickBar
    tracks — D-Bus match rules can't filter down to "only names I've
    registered items under," so every firing is checked against the registry).
    An empty `new_owner` means that name's previous owner disconnected;
    `sni_registry_remove_by_bus_name()` then removes every item registered
    under that bus name (a single client registering more than one item is
    rare but possible, so this isn't just `sni_registry_remove()` on one
    (bus_name, path) pair). Without this, closing an app would leave its icon
    stuck in the tray forever with no other signal ever telling stickBar it's
    gone — this path has no precedent elsewhere in the codebase to lean on and
    was the one part of this feature confirmed only via a live
    start-app/quit-app/watch-icon-disappear test, not by reasoning about the
    spec text.
- **Bus-name/object-path splitting**: `RegisteredStatusNotifierItems` and the
  `StatusNotifierItem{,Un}Registered` signals give strings like
  `":1.30/StatusNotifierItem"` or `":1.52/org/ayatana/NotificationItem/steam"` —
  confirmed both forms occur on real items. Rule: the first `/` splits bus name
  from path; no `/` at all means the whole string is the bus name and the path
  defaults to `/StatusNotifierItem`.
- **IconPixmap byte format — verified pixel-by-pixel against real wire data, not
  assumed.** SNI pixmap bytes are ARGB32 in network byte order (big-endian); a
  per-pixel `ntohl()` on the raw 4 bytes recovers `0xAARRGGBB` in host-native
  form, which is exactly what cairo's `CAIRO_FORMAT_ARGB32` wants stored as a
  host-native `uint32_t` (`dbus/sni_icon.c`). This was confirmed correct by
  dumping raw wire bytes and the converted output side by side and checking the
  alpha channel traces out a coherent icon glyph shape.
  - **Discovered while verifying this, not a bug**: some real apps (EasyEffects
    included, via its `-symbolic`-suffixed icon name) ship IconPixmap as a
    flat-colored *mask* — every opaque pixel carries the *exact same* dim,
    low-contrast RGB, meant to be recolored at display time to the desktop
    theme's foreground color. The reference Quickshell bar renders this same
    item bright/legible because Qt/KDE's full icon-theme integration recolors it
    via `IconName`, not the raw pixmap. This project doesn't do real theme
    lookups, so instead `sni_icon_convert()` now applies a cheap heuristic
    directly on the pixmap bytes (`detect_symbolic_mask()` in
    `dbus/sni_icon.c`, run once per icon before the premultiply pass): sample
    the straight RGB of every pixel above alpha 40/255 (low-alpha edge pixels
    are noisy and shouldn't count), and if the max-min range across R/G/B among
    those samples is within 12/255 *and* the shared color's Rec. 601 luminance
    is under 40%, treat it as a mask and override every pixel's RGB to
    `COLORS.text` before premultiplying, leaving alpha (the glyph shape)
    untouched. Scoped to this IconPixmap path only, deliberately not applied to
    `dbus/icon_theme.c`'s PNG-file fallback path — a flat-colored mask is a
    specifically IconPixmap-shaped problem, and applying the same heuristic to
    a normal multi-color raster app logo loaded from an icon theme file risks
    wrongly recoloring a legitimately monochrome icon. Confirmed working live:
    with Quickshell down and stickBar as its own Watcher,
    EasyEffects' tray icon renders in this bar's light `COLORS.text` instead of
    its dim baked-in color, while Steam's icon (no IconPixmap at all, resolved
    via the PNG fallback instead) is unaffected.
  - Premultiplication: cairo wants premultiplied alpha; SNI pixmaps are
    conventionally *not* premultiplied on the wire, so `sni_icon_convert()`
    premultiplies unconditionally — also confirmed correct via the same
    byte-level check.
- **DBusMenu's `Event` method is fire-and-forget (`NoReply`)** — confirmed
  against the real `com.canonical.dbusmenu` interface definition. Sent via
  `sd_bus_message_set_expect_reply(msg, 0)` + `sd_bus_send()`, not
  `sd_bus_call_method()` (which blocks waiting for a reply that a `NoReply`
  method may never send — and blocking at all here would freeze the whole bar's
  event loop until the target app responds, since this is a single-threaded
  process). `sni_item_activate()`'s `Activate()` call uses the same fire-and-forget
  `sd_bus_send()` pattern for the same reason, even though `Activate` isn't
  `NoReply` — any eventual reply just arrives with no pending-call slot
  registered to receive it and is silently dropped by sd-bus, which is normal,
  not an error.
- **`GetLayout`'s `recursionDepth=1`** naturally produces the flat,
  non-recursive entry list the reference `TrayContextMenu.qml` renders — no
  extra filtering needed. A live `GetLayout(0, 1, [])` against a real menu
  confirmed the shape: entries without a `"type"` property are normal clickable
  items, `"type":"separator"` marks a separator, and `"enabled"` **defaults to
  `true` if the property is absent** (matters for correctness, not just a spec
  footnote — most real entries omit it).
- **Tray menu styling**: neutral grey (`overlay0` border, `overlay2` hover bar) per
  the user, since the tray has no accent colour of its own.
  - **Header**: the item's own tray icon (the pre-scaled 20px pixmap the bar
    already holds), its name (SNI `Title`, now fetched, falling back to
    `Id`) and the entry count.
  - **Separators become cards**: each run of entries between separators is
    a `base` card (leading/trailing/repeated separators make no empty
    card). Steam's 16-entry menu reads as 4 cards.
  - **Entries**: hover fills the row (`surface0`) with a 3px bar on the
    left; disabled entries are dimmed and no longer highlight. Now parsed in
    `dbus/dbusmenu.c`: `toggle-type`/`toggle-state` (drawn as checkbox /
    radio glyphs, a column reserved only if any entry has one),
    `children-display: submenu` (a `›` marker), and labels have their access-key underscores stripped
    (`_Quit` -> `Quit`, `__` -> `_`); they used to be drawn as-is.
  - Tested by `tests/test_tray.py` against `tests/lib/fake_tray.py`, a Gio
    StatusNotifierItem + dbusmenu that only logs clicks. **It registers
    asynchronously on purpose**: the watcher (this bar's
    `sni_watcher_service.c`) fetches the item's properties *while handling*
    `RegisterStatusNotifierItem`, so a client that blocks on that call
    deadlocks until the D-Bus timeout -- which is also why a hung tray app
    can freeze the bar (see [limitations.md](../limitations.md)).
- **The has-menu-overrides-button click rule** (`modules/tray/tray.c`'s
  `tray_handle_click()`): re-reading the reference `Tray.qml`'s actual
  `onClicked` handler shows that if an item has a menu, **any** click — left or
  right — opens it; `Activate()` only ever fires when the item has **no** menu
  at all, and only then on left-click. This is easy to get backwards (assuming
  "left activates, right opens the menu" by convention) — menu presence
  overrides button entirely. Both live test items here (EasyEffects, Steam) have
  menus, so left/right click look identical in manual testing; the `Activate()`
  path exists but isn't exercised without a menu-less item.
