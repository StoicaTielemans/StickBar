#ifndef CUSTOMBAR_AUDIO_VOLUME_STYLE_H
#define CUSTOMBAR_AUDIO_VOLUME_STYLE_H

#include <stdbool.h>
#include <stdint.h>

#include "config/config.h"

// How volume looks, shared by the volume box (modules/volume), the device
// dropdown and the mixer (audio/) -- each used to have its own copy.

// The speaker icon for `percent`: muted (or 0%), <34%, <67%, else --
// VolumeWidget.qml's icon() thresholds exactly.
uint32_t volume_level_glyph(int percent, bool muted);

// A volume meter's 3-stop colours: all `overlay0` when muted; otherwise the
// accent, with the top end turning `peach` once `percent` goes past 100% --
// the meters' full end; pulse allows up to PULSE_MAX_VOLUME_PERCENT, and a
// full meter with a peach end reads as "boosted past 100%".
void volume_meter_gradient(Color out[3], Color accent, bool muted, int percent);

// The percent a pointer at `x` sets on a slider spanning track_x..+track_w
// that runs 0..max_percent -- clamped to the track's ends.
int volume_slider_percent(double x, double track_x, double track_w, int max_percent);

#endif
