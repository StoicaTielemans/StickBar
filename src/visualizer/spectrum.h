#ifndef CUSTOMBAR_VISUALIZER_SPECTRUM_H
#define CUSTOMBAR_VISUALIZER_SPECTRUM_H

// Pure math, no PipeWire types (same split as dbus/sni_icon.c vs. the D-Bus
// files around it): turns a window of mono samples into log-spaced
// frequency-band levels for the media dropdown's spectrum bars.

// Samples per analysis window. A power of two (the FFT is plain radix-2). At
// 48kHz that's ~85ms of audio and ~11.7Hz per FFT bin -- fine enough that the
// lowest of the 82 log-spaced bands (50Hz up) mostly get distinct bins (2048
// gave 23Hz bins, and the bottom dozen columns moved as one).
#define SPECTRUM_FFT_SIZE 4096

// How many bands: one per dot column of the media dropdown's braille graph
// (41 cells x 2 dot columns -- media_popup.c asserts it matches).
#define SPECTRUM_BAR_COUNT 82

// Runs the FFT over `samples` (SPECTRUM_FFT_SIZE mono floats, -1..1, oldest
// first, at `rate` Hz) and keeps the magnitudes for spectrum_bands() below.
// This is the expensive half; call it once per frame.
void spectrum_analyze(const float *samples, float rate);

// Groups the last spectrum_analyze()'s bins into `count` log-spaced bands and
// writes one 0..1 level per band into `out` -- raw, unsmoothed: smoothing over
// time is the caller's job (visualizer.c), since it depends on the frame rate
// and this file deliberately doesn't know it. Cheap next to the FFT.
void spectrum_bands(int count, float *out);

#endif
