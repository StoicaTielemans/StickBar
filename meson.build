project('stickBar', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',      # accept GNU C extensions used in a couple of places; note this
                         # does NOT expose glibc-only APIs like memfd_create() on its
                         # own -- files needing those still #define _GNU_SOURCE themselves
    'warning_level=2',
  ],
)

wayland_client = dependency('wayland-client')
# wayland-cursor is intentionally not linked yet: this pass doesn't set a pointer
# cursor image on hover (the bar still gets pointer input/clicks correctly without
# it, just no visible cursor change) -- add it back when that's implemented.
cairo          = dependency('cairo')
pangocairo     = dependency('pangocairo')
cjson          = dependency('libcjson')   # pkg-config module name is "libcjson", not "cjson"
libsystemd     = dependency('libsystemd') # pkg-config module name is "libsystemd", not
                                           # "sd-bus" -- provides sd-bus, the D-Bus client
                                           # library the tray module uses to talk to
                                           # StatusNotifierItem/DBusMenu
libpulse       = dependency('libpulse')   # PulseAudio client library, used by
                                           # src/audio/pulse_client.c to talk to
                                           # pipewire-pulse (or any real PulseAudio
                                           # server) for the volume/mic modules --
                                           # see CLAUDE.md's "PulseAudio / volume &
                                           # mic" section for why libpulse rather
                                           # than raw libpipewire
gdk_pixbuf     = dependency('gdk-pixbuf-2.0') # image decoding (PNG/JPEG/etc.), used
                                           # by src/mpris/mpris_art.c to decode a
                                           # local MPRIS mpris:artUrl into a cairo
                                           # surface for the media popup's
                                           # thumbnail -- cairo alone only decodes
                                           # PNG natively, which would miss most
                                           # real-world (commonly JPEG) cover art;
                                           # see CLAUDE.md's "Media (MPRIS)" section

wayland_scanner = find_program('wayland-scanner')

# wayland-scanner turns a protocol XML into a C header (the wl_interface/struct
# declarations client code includes) and a matching .c file (the wl_interface
# definitions those declarations point at) — both are needed to link.
wl_scanner_client_header = generator(wayland_scanner,
  output: '@BASENAME@-client-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)
wl_scanner_private_code = generator(wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

# The layer-shell protocol's get_popup request references xdg_popup. Compiling
# against just a forward declaration works fine (no popups are actually used in this
# pass), but wayland-scanner's generated *-protocol.c for layer-shell still emits a
# reference to the `xdg_popup_interface` symbol as part of that request's wl_message
# signature -- so linking fails without xdg-shell.xml's own generated code to
# actually define it, even though nothing here calls into xdg_popup itself. Pulled
# from the system wayland-protocols package rather than vendored, since (unlike
# wlr-layer-shell) it's a stable upstream protocol that's reliably installed
# alongside wayland-protocols.
wayland_protocols_dep = dependency('wayland-protocols')
xdg_shell_xml = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

protocol_xmls = files('protocols/wlr-layer-shell-unstable-v1.xml') + files(xdg_shell_xml)

generated_headers = wl_scanner_client_header.process(protocol_xmls)
generated_sources = wl_scanner_private_code.process(protocol_xmls)

sources = files(
  'src/main.c',
  'src/config/config.c',
  'src/config/glyphs.c',
  'src/wayland/registry.c',
  'src/wayland/event_loop.c',
  'src/wayland/output.c',
  'src/wayland/pointer.c',
  'src/wayland/shm_buffer.c',
  'src/bar/bar_surface.c',
  'src/render/render.c',
  'src/render/module_box.c',
  'src/hyprland/ipc_request.c',
  'src/hyprland/ipc_events.c',
  'src/hyprland/dispatch.c',
  'src/modules/workspaces/workspaces.c',
  'src/modules/clock/clock.c',
  'src/modules/clock/clock_calendar_popup.c',
  'src/dbus/dbus_loop.c',
  'src/dbus/sni_watcher.c',
  'src/dbus/sni_watcher_service.c',
  'src/dbus/sni_registry.c',
  'src/dbus/sni_icon.c',
  'src/dbus/icon_theme.c',
  'src/dbus/dbusmenu.c',
  'src/bar/popup_surface.c',
  'src/modules/tray/tray.c',
  'src/modules/tray/tray_context_menu.c',
  'src/audio/pulse_client.c',
  'src/audio/device_popup.c',
  'src/modules/volume/volume.c',
  'src/modules/mic/mic.c',
  'src/sysmon/sysmon_client.c',
  'src/sysmon/sysmon_info_popup.c',
  'src/modules/cpu/cpu.c',
  'src/modules/gpu/gpu.c',
  'src/modules/mem/mem.c',
  'src/mpris/mpris_client.c',
  'src/mpris/mpris_art.c',
  'src/modules/media/media.c',
  'src/modules/media/media_popup.c',
  'src/network/network_client.c',
  'src/modules/network/network.c',
  'src/modules/network/network_info_popup.c',
  'src/modules/powermenu/arch_logo.c',
  'src/modules/powermenu/power_menu.c',
)

executable('stickBar',
  sources + generated_sources + generated_headers,
  dependencies: [wayland_client, cairo, pangocairo, cjson, libsystemd, libpulse, gdk_pixbuf],
  include_directories: include_directories('src'),
)
