# D-Bus / system tray

Files: `src/dbus/` (`mod.rs` the thread, `watcher.rs`, `sni.rs`, `sni_icon.rs`,
`icon_theme.rs`, `dbusmenu.rs`) and `src/modules/tray/` (`mod.rs` the icon row, `menu.rs` the
dropdown). Two freedesktop protocols on the session bus: StatusNotifierItem (SNI, the icons)
and DBusMenu (their menus). Both were checked against this machine's real tray items
(`busctl --user ...` against EasyEffects, Steam, Discord, CurseForge), not only the spec
text; several details below only became clear that way.

## The `dbus` thread

- **zbus 5, pinned:** zbus 6 dropped the blocking API and reworked its runtime. No default
  features, just `async-io`.
- **One `dbus` thread owns the connection** (MPRIS uses it too, [media.md](media.md)). It
  runs one async loop over the merged streams (Watcher method calls, `NameOwnerChanged`,
  every item's `NewIcon`, the Watcher's signals in Host mode) and the main thread's commands,
  one at a time. The main loop gets `TrayEvent`s on a calloop channel and never waits for an
  app. **Calls time out after 3 s.** zbus's executor is ticked by this thread
  (`internal_executor(false)`), which saves zbus's own thread; its runtime still starts
  `async-io`, which blocks SIGUSR1 like every thread.
- **Stream back-pressure:** a full zbus stream makes zbus's reader wait, which also holds up
  replies, so the queues are 256 deep. The worst case is a timed-out call on the `dbus` thread.
- **Never read big D-Bus values as a generic `zvariant::Value`.** Inside a `Value`, an `ay` is
  one `Value` per byte: CurseForge's 256x256 `IconPixmap` (262 KB) became several MB on every
  add, freeing it raised glibc's adaptive mmap threshold, and the calendar's temporary
  full-size image then stayed resident (`test_calendar`: +6.1 MB instead of +3.8 MB). `GetAll`
  is read by a small serde visitor (`ItemProps` in `sni.rs`): `IconPixmap` typed, the text
  properties normally, everything else skipped.

## Watcher or Host, decided at startup

- **The bar is the StatusNotifierWatcher itself if it can be** (`watcher::start`): it asks for
  `org.kde.StatusNotifierWatcher` (and best-effort `org.freedesktop.StatusNotifierWatcher`, for
  hosts that look for that name), and serves `/StatusNotifierWatcher` so items register with
  it directly. If the name is taken (the user's running bar, or Quickshell), it registers as a
  **Host** with whichever Watcher exists. If neither works, the tray stays empty for the run
  ([limitations.md](../limitations.md)). Nothing outside `watcher.rs` knows which mode is on.
- **The Watcher is served by hand** from a method-call stream (register item/host,
  `Properties.Get/GetAll`, `Introspect`, `Ping`, `UnknownMethod`), not with zbus's
  `#[interface]` macro. zbus 5 only starts its object server when something is served through
  it, so nothing answers twice. **`RegisterStatusNotifierItem` is answered before the item is
  read**, so a client blocking on the reply can't deadlock with the bar.
- **Both registration conventions are accepted**: an argument starting with `/` is an object
  path on the caller's own unique name (Steam); otherwise it's a bus name with the path
  `/StatusNotifierItem` (the KDE convention, EasyEffects).
- **Splitting `RegisteredStatusNotifierItems` entries** (`split_bus_name_path`): strings look
  like `":1.30/StatusNotifierItem"` or `":1.52/org/ayatana/NotificationItem/steam"` (both seen
  on real items). The first `/` splits bus name from path; no `/` means the whole string is
  the bus name and the path is `/StatusNotifierItem`.
- **Items that go away:** `NameOwnerChanged` is followed in both modes. It fires for every
  name change on the bus (a match rule can't narrow it to "names I track"), so each one is
  checked against the registry. An empty new owner removes every item under that bus name
  (one client can register several). As the Watcher, the bar emits
  `StatusNotifierItemUnregistered`. Without this, a quit app's icon would stay forever.

## Icons

- **`IconPixmap` is ARGB32 in network byte order** (big-endian); converting each pixel from
  big-endian gives `0xAARRGGBB`, what cairo's `ARGB32` stores natively. Pixmaps aren't
  premultiplied on the wire, so `sni_icon.rs` premultiplies. Both checked by dumping real wire
  bytes next to the converted output.
- **Flat-coloured symbolic masks are recoloured** (`detect_symbolic_mask`). Some apps (e.g.
  EasyEffects, with a `-symbolic` icon name) send a mask where every opaque pixel has the same
  dim RGB, meant to be recoloured to the theme's foreground; Qt does that through `IconName`.
  Here: sample every pixel above alpha 40/255; if R/G/B stay within 12/255 of each other and
  the colour's Rec. 601 luminance is under 40 %, every pixel's RGB becomes `COLORS.text`,
  alpha untouched. Only for `IconPixmap`: a PNG from the icon-theme fallback is a real logo
  and could legitimately be monochrome.
- **No `IconPixmap`?** `icon_theme::lookup` finds a PNG ([limitations.md](../limitations.md)
  says how far that goes). Steam's icon comes this way, through its `IconThemePath`.

## Menus

- **The has-menu rule** (`App::tray_click`, from the reference `Tray.qml`'s `onClicked`): if an
  item has a menu, **any** click (left or right) opens it; `Activate()` only fires for an item
  with **no** menu, on left-click. Easy to get backwards. Every real item here has a menu, so
  the `Activate()` path isn't exercised by hand or by a test.
- **The menu opens when its entries arrive.** A click sends `FetchMenu` and remembers the icon;
  the answer opens the dropdown, a newer click makes an older answer stale, and an empty menu
  opens nothing.
- **`GetLayout` with `recursionDepth=1`** gives the flat list the reference
  `TrayContextMenu.qml` renders. Entries without `"type"` are normal items,
  `"type":"separator"` is a separator, and **`"enabled"` defaults to true when absent** (most
  real entries omit it).
- **`Event` and `Activate` are sent with `NoReplyExpected`**: DBusMenu's `Event` is a `NoReply`
  method, and nothing useful comes back from `Activate` either.
- **Look**: neutral grey (`overlay0` border, `overlay2` hover bar) per the user, since the tray
  has no accent of its own. The header has the item's own icon, its name (SNI `Title`, falling
  back to `Id`) and the entry count. Each run of entries between separators is a `base` card
  (no empty cards; Steam's 16 entries read as 4 cards). Hover fills the row (`surface0`) with
  a 3 px bar on the left; disabled entries are dimmed and inert. `toggle-type`/`toggle-state`
  draw checkbox/radio glyphs (a column reserved only if any entry has one),
  `children-display: submenu` gets a `›`, and access-key underscores are stripped (`_Quit` ->
  `Quit`, `__` -> `_`).
- **Hand cursor:** each icon is a `ClickTarget` with `Action::TrayIcon(id)`.

## Testing

`tests/test_tray.py` runs against `tests/lib/fake_tray.py`, a Gio StatusNotifierItem plus
DBusMenu that only logs clicks. The user's running bar owns the Watcher names, so a test bar
is in Host mode; `dbus-run-session -- python3 tests/test_tray.py` gives it a private bus,
where it becomes the Watcher.
