#ifndef CUSTOMBAR_AUDIO_MIXER_POPUP_H
#define CUSTOMBAR_AUDIO_MIXER_POPUP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "config/config.h"
#include "wayland/context.h"

// Per-application volume mixer dropdown, opened by middle-clicking the volume
// module (modules/volume/volume.c). One row per app currently playing audio
// (a PA sink input, see pulse_client.h's PulseStream): the app's name, a
// volume slider (click or drag to set, scroll to step), the percent, and a
// mute toggle icon. Same right-anchored-under-the-trigger positioning and
// per-output click-catcher/outside-click-dismiss shape as
// audio/device_popup.c, its sibling -- but unlike that popup, this one
// resizes itself live as apps start/stop playing while it's open, since app
// streams come and go far more often than audio devices do.
void mixer_popup_open(Output *output, Color accent,
                       double box_x, double box_y, double box_w, double box_h);

// Closes the mixer if open (no-op otherwise).
void mixer_popup_close(void);

// Re-reads pulse_client's stream cache and redraws (resizing if the row count
// changed), if the mixer is open. Safe to call unconditionally after every
// pulse_client_pump().
void mixer_popup_refresh_if_open(void);

bool mixer_popup_is_menu_surface(struct wl_surface *surface);
bool mixer_popup_is_catcher_surface(struct wl_surface *surface);

// Pointer routing from wayland/pointer.c. A left press on a row's slider sets
// that app's volume to the clicked position and starts a drag (motion keeps
// adjusting it until mixer_popup_handle_release(), which relies on Wayland's
// implicit pointer grab still delivering motion to this surface while the
// button is held); a left press on the mute icon toggles that app's mute.
// Scrolling over a row steps that app's volume by PULSE_VOLUME_STEP_PERCENT.
void mixer_popup_handle_press(double x, double y, uint32_t button);
void mixer_popup_handle_motion(double x, double y);
void mixer_popup_handle_release(void);
void mixer_popup_handle_scroll(double x, double y, int direction);

#endif
