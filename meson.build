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
libpipewire    = dependency('libpipewire-0.3') # direct PipeWire client library
                                           # (unlike libpulse above, which only
                                           # ever talks the PulseAudio-compatible
                                           # protocol for volume/mic control),
                                           # used by src/pipewire/pipewire_client.c
                                           # to monitor the live stream graph for
                                           # the privacy indicator module (mic/
                                           # webcam/screen-share) -- see
                                           # CLAUDE.md's "Privacy indicator"
                                           # section for why this needed the raw
                                           # PipeWire API rather than libpulse

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
# wp_viewporter (stable) lets a popup's full-screen click-catcher surfaces be
# backed by a 1x1 buffer stretched to the output size instead of a full-size
# transparent one -- see bar/popup_surface.c's popup_surface_create_catcher().
viewporter_xml = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'stable/viewporter/viewporter.xml'

protocol_xmls = files('protocols/wlr-layer-shell-unstable-v1.xml') + files(xdg_shell_xml) + files(viewporter_xml)

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
  'src/render/braille_graph.c',
  'src/render/meter_bar.c',
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
  'src/audio/mixer_popup.c',
  'src/modules/volume/volume.c',
  'src/modules/mic/mic.c',
  'src/sysmon/sysmon_client.c',
  'src/modules/cpu/cpu.c',
  'src/modules/gpu/gpu.c',
  'src/modules/gpu/gpu_popup.c',
  'src/modules/cpu/cpu_popup.c',
  'src/modules/mem/mem.c',
  'src/modules/mem/mem_popup.c',
  'src/mpris/mpris_client.c',
  'src/modules/media/media.c',
  'src/modules/media/media_popup.c',
  'src/network/network_client.c',
  'src/modules/network/network.c',
  'src/modules/network/network_info_popup.c',
  'src/modules/powermenu/arch_logo.c',
  'src/modules/powermenu/power_menu.c',
  'src/pipewire/pipewire_client.c',
  'src/modules/privacy/privacy.c',
  'src/modules/privacy/privacy_info_popup.c',
  'src/eq/eq_state.c',
  'src/eq/eq_curve.c',
  'src/modules/eq/eq.c',
  'src/modules/eq/eq_popup.c',
)

# dlopen()/dlsym() for loading NVIDIA's NVML at runtime (sysmon/sysmon_client.c).
# On glibc >= 2.34 these live in libc itself and libdl is an empty stub, so this
# is optional and usually resolves to nothing at all.
libdl = meson.get_compiler('c').find_library('dl', required: false)

executable('stickBar',
  sources + generated_sources + generated_headers,
  dependencies: [wayland_client, cairo, pangocairo, cjson, libsystemd, libpulse, libpipewire, libdl],
  include_directories: include_directories('src'),
)
