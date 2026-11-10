#ifndef CUSTOMBAR_PIPEWIRE_PIPEWIRE_CLIENT_H
#define CUSTOMBAR_PIPEWIRE_PIPEWIRE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Connects directly to PipeWire (not via the PulseAudio-compatible protocol
// audio/pulse_client.c uses -- volume/mic *control* has no equivalent need to
// see the raw stream graph, but "which app is reading the mic/webcam/screen
// right now" does) and starts a background thread (pw_thread_loop) that owns
// the connection for the rest of the process's life. Same shape as
// pulse_client_init(): returns false (with a stderr message) on failure, and
// every accessor below degrades to an empty/false result rather than the bar
// failing to start -- the privacy module just stays permanently collapsed for
// this run.
bool pipewire_client_init(void);

void pipewire_client_shutdown(void);

// An eventfd, signaled from the background thread every time the locally
// cached node/link/client graph changes. Register this on the caller's epoll
// instance; on readiness, call pipewire_client_pump().
int pipewire_client_notify_fd(void);

// Drains the eventfd, refreshes the cached privacy state, and reports what
// changed since the last pump -- most graph changes (sinks, filters, links,
// the visualizer's own captures) change neither, and then nothing needs
// redrawing.
#define PIPEWIRE_PRIVACY_CHANGED 1u  // mic/webcam/screen-share in use
#define PIPEWIRE_PLAYBACK_CHANGED 2u // playback streams or their clients' pids
unsigned pipewire_client_pump(void);

typedef enum {
    PRIVACY_CATEGORY_MIC,
    PRIVACY_CATEGORY_WEBCAM,
    PRIVACY_CATEGORY_SCREENSHARE,
} PrivacyCategory;

// Whether any currently-tracked stream falls into each category. A
// "Stream/Input/Audio" node with state == PW_NODE_STATE_RUNNING is a mic
// recording -- confirmed live against a real `parecord` capture during
// development. A "Stream/Input/Video" node is either a webcam or a
// screen-share consumer; which one is resolved by following the PipeWire
// link feeding it back to its source node and checking *that* node's own
// media.class (see pipewire_client.c's video_node_is_webcam() for the exact
// heuristic and its documented caveat -- this half is reasoned from
// PipeWire's own node-role conventions, not independently verified against
// real webcam hardware or a real screen-share session on this machine).
// As of the last pipewire_client_pump() -- cached, so cheap to call per frame.
bool pipewire_client_mic_active(void);
bool pipewire_client_webcam_active(void);
bool pipewire_client_screenshare_active(void);

// Flattened list of every active stream across all three categories, for the
// right-click dropdown. 0 <= index < pipewire_client_entry_count().
int pipewire_client_entry_count(void);

// The object.serial of a playback stream (Stream/Output/Audio node) belonging
// to process `pid` or one of its child processes -- a media player's own
// audio, for its graph in the media dropdown (visualizer/visualizer.c captures
// it). Prefers a running stream when the app has several. False if none.
// Reads /proc, so call it on changes (popup open, PipeWire graph changes),
// not per frame.
bool pipewire_find_playback_for_pid(int pid, uint32_t *serial_out);
bool pipewire_client_entry(int index, PrivacyCategory *category_out,
                            char *app_name_out, size_t app_name_out_len);

#endif
