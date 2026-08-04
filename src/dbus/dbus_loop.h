#ifndef CUSTOMBAR_DBUS_DBUS_LOOP_H
#define CUSTOMBAR_DBUS_DBUS_LOOP_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

// Connects to the D-Bus session bus. Returns NULL (with a stderr message) on
// failure -- callers should degrade gracefully (the tray module disables itself
// for this run) rather than failing the whole bar, matching how
// hyprland/ipc_events.c already treats a missing/broken Hyprland connection as
// optional rather than fatal.
sd_bus *dbus_loop_connect(void);

// The connection's underlying fd, to register on the caller's epoll instance.
// sd-bus's *interest* in this fd (readable/writable) changes over time --
// dbus_loop_pump() re-derives and re-applies it after every pump, so the initial
// registration here only needs to start with EPOLLIN.
int dbus_loop_fd(sd_bus *bus);

// A plain CLOCK_MONOTONIC timerfd (TFD_NONBLOCK|TFD_CLOEXEC), created disarmed.
// sd-bus tracks its own internal deadlines (e.g. method call timeouts) that need
// to fire even though this project drives sd-bus from an external epoll loop
// instead of its own default one -- dbus_loop_pump() arms/disarms this to match
// whatever sd_bus_get_timeout() reports after each pump. Returns -1 on failure.
int dbus_loop_create_timeout_fd(void);

// Call whenever epoll reports activity on the bus fd or the timeout fd. Processes
// all currently-available D-Bus work, then re-arms both `epoll_fd`'s interest in
// `dbus_fd` and `timeout_fd`'s deadline to match sd-bus's current needs. Draining
// the timeout fd's own counter (if that's what fired) is the caller's
// responsibility, same as the clock's timerfd elsewhere in main.c.
void dbus_loop_pump(sd_bus *bus, int epoll_fd, int dbus_fd, int timeout_fd);

// sd_bus_flush_close_unref(): sends any outstanding outgoing messages, closes the
// connection, and frees it. Safe to call with bus == NULL (no-op).
void dbus_loop_disconnect(sd_bus *bus);

#endif
