#ifndef CUSTOMBAR_AUDIO_PULSE_CLIENT_H
#define CUSTOMBAR_AUDIO_PULSE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct PulseNode PulseNode; // opaque

// Shared with modules/volume and modules/mic so the scroll step and the clamp
// ceiling live in exactly one place. PULSE_MAX_VOLUME_PERCENT matches the
// reference AudioMonitor.qml's own maxVolume: 2.0 (200%) -- not capped at
// 100% deliberately, mirroring that choice for parity rather than guessing at
// a "safer" limit.
#define PULSE_VOLUME_STEP_PERCENT 5
#define PULSE_MAX_VOLUME_PERCENT 200

// Connects to the PulseAudio-compatible server (pipewire-pulse on this project's
// target system, but any real PulseAudio server works identically -- this file
// never assumes PipeWire specifically) and starts a background thread
// (pa_threaded_mainloop) that owns the connection for the rest of the process's
// life. Returns false (with a stderr message) on failure -- callers should
// degrade gracefully, same convention as dbus_loop_connect()/hyprland_events_connect():
// the volume/mic modules just render "--" and stay non-interactive for this run
// rather than the whole bar failing to start.
bool pulse_client_init(void);

void pulse_client_shutdown(void);

// An eventfd, signaled from the background thread every time the locally
// cached sink/source state changes (volume, mute, device list, or default
// device). Register this on the caller's epoll instance; on readiness, call
// pulse_client_pump() (drains the eventfd) and then redraw.
int pulse_client_notify_fd(void);
void pulse_client_pump(void);

// --- Default sink (speaker) ---
// All of these read/mutate the locally cached snapshot under the same lock the
// background thread uses to update it (pa_threaded_mainloop's own lock, reused
// here as a general-purpose mutex rather than introducing a second one) --
// safe to call from the main thread at any time, including before the
// connection is ready (reads return 0/false; writes are silently dropped).
int pulse_default_sink_volume_percent(void);
bool pulse_default_sink_muted(void);
void pulse_default_sink_adjust_volume(int delta_percent);
void pulse_default_sink_toggle_mute(void);

// --- Default source (microphone) ---
int pulse_default_source_volume_percent(void);
bool pulse_default_source_muted(void);
void pulse_default_source_adjust_volume(int delta_percent);
void pulse_default_source_toggle_mute(void);

// --- Device enumeration, for the dropdown popup ---
int pulse_sink_count(void);
PulseNode *pulse_sink_get(int index); // 0 <= index < pulse_sink_count()
int pulse_source_count(void);
PulseNode *pulse_source_get(int index); // 0 <= index < pulse_source_count()

const char *pulse_node_description(PulseNode *node);
int pulse_node_volume_percent(PulseNode *node);
bool pulse_node_muted(PulseNode *node);
bool pulse_node_is_default(PulseNode *node);

void pulse_node_set_default(PulseNode *node);
void pulse_node_toggle_mute(PulseNode *node);

#endif
