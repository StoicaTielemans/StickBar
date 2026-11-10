#include "visualizer/spectrum.h"

#include <math.h>
#include <stdbool.h>

// Band edges: SPECTRUM_BAR_COUNT bands spaced evenly on a log scale between
// these two -- the way hearing (and every cava-style visualizer) spaces
// frequencies. Below ~50Hz is mostly rumble a laptop speaker can't play
// anyway; above ~12kHz there's little energy in most music, so a bar there
// would just sit flat.
#define BAND_LO_HZ 50.0f
#define BAND_HI_HZ 12000.0f

// dB range mapped onto 0..1 bar height (after the tilt below). Picked by ear
// against real music at normal listening volume: quieter passages still move,
// loud ones only occasionally pin the top.
#define FLOOR_DB -62.0f
#define CEIL_DB -14.0f

// Music's spectrum falls off roughly 3dB per octave going up, so without a
// tilt the left (bass) bars would be tall and the right ones nearly dead.
// Added per octave above TILT_PIVOT_HZ (subtracted below it).
#define TILT_DB_PER_OCTAVE 3.0f
#define TILT_PIVOT_HZ 500.0f

// spectrum_analyze()'s output, read by spectrum_bands(): |X[k]| scaled to the
// amplitude (0..1) of a sine on bin k, for the non-redundant half.
static float g_mag[SPECTRUM_FFT_SIZE / 2];
static float g_rate = 48000.0f;

// The input is real, so the SPECTRUM_FFT_SIZE-point transform is done as a half-size
// complex FFT over the samples packed in pairs (even -> real, odd -> imag),
// plus one O(N) pass that separates the two halves again -- half the
// butterflies of a plain complex FFT. Profiling the bar showed the FFT was
// most of the visualizer's per-frame CPU: run 15 times a second its tables and
// buffers are cache-cold each time, so it cost ~0.13ms against ~0.03ms in a hot
// benchmark.
#define HALF (SPECTRUM_FFT_SIZE / 2)

static float g_window[SPECTRUM_FFT_SIZE];
static unsigned g_bitrev[HALF];
// e^(-2*pi*i*j/HALF) for the HALF-point FFT's butterflies, and
// e^(-2*pi*i*k/SPECTRUM_FFT_SIZE) for the real-input fix-up pass. Tables
// rather than a running product per butterfly: no long dependency chain, and
// no accumulated rounding.
static float g_tw_re[HALF / 2], g_tw_im[HALF / 2];
static float g_post_re[HALF], g_post_im[HALF];
static bool g_tables_ready = false;

// Hann window (tames the spectral leakage a plain rectangular window smears
// across every bar), the bit-reversal permutation and the twiddle tables --
// all fixed for a fixed FFT size, so computed once.
static void init_tables(void) {
    for (unsigned i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        g_window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(SPECTRUM_FFT_SIZE - 1));
    }
    unsigned bits = 0;
    while ((1u << bits) < HALF) {
        bits++;
    }
    for (unsigned i = 0; i < HALF; i++) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; b++) {
            if (i & (1u << b)) {
                r |= 1u << (bits - 1 - b);
            }
        }
        g_bitrev[i] = r;
        g_post_re[i] = cosf(-2.0f * (float)M_PI * (float)i / (float)SPECTRUM_FFT_SIZE);
        g_post_im[i] = sinf(-2.0f * (float)M_PI * (float)i / (float)SPECTRUM_FFT_SIZE);
    }
    for (unsigned j = 0; j < HALF / 2; j++) {
        g_tw_re[j] = cosf(-2.0f * (float)M_PI * (float)j / (float)HALF);
        g_tw_im[j] = sinf(-2.0f * (float)M_PI * (float)j / (float)HALF);
    }
    g_tables_ready = true;
}

// In-place iterative radix-2 complex FFT of HALF points.
static void fft_half(float *re, float *im) {
    for (unsigned i = 0; i < HALF; i++) {
        unsigned j = g_bitrev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (unsigned len = 2; len <= HALF; len <<= 1) {
        unsigned half_len = len / 2, step = HALF / len; // twiddle k*step for this stage
        for (unsigned start = 0; start < HALF; start += len) {
            for (unsigned k = 0; k < half_len; k++) {
                unsigned a = start + k, b = a + half_len;
                float wr = g_tw_re[k * step], wi = g_tw_im[k * step];
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

void spectrum_analyze(const float *samples, float rate) {
    if (!g_tables_ready) {
        init_tables();
    }

    // z[n] = x[2n] + i*x[2n+1], windowed.
    static float zr[HALF], zi[HALF];
    for (unsigned n = 0; n < HALF; n++) {
        zr[n] = samples[2 * n] * g_window[2 * n];
        zi[n] = samples[2 * n + 1] * g_window[2 * n + 1];
    }
    fft_half(zr, zi);

    // |X[k]| * this = the amplitude (0..1) of a sine sitting exactly on bin k:
    // a one-sided spectrum doubles it, and the Hann window's coherent gain of
    // 0.5 halves the N it would otherwise be scaled by. So a full-scale sine
    // reads as 0dB, and FLOOR_DB/CEIL_DB above are plain dBFS.
    const float amp_scale = 4.0f / (float)SPECTRUM_FFT_SIZE;

    // Untangle the even/odd halves: with Zc = conj(Z[HALF-k]) (Z[HALF] = Z[0]),
    //   E = (Z[k] + Zc) / 2,  O = (Z[k] - Zc) / 2i,  X[k] = E + W^k * O.
    for (unsigned k = 0; k < HALF; k++) {
        unsigned m = k ? HALF - k : 0;
        float cr = zr[m], ci = -zi[m];
        float er = 0.5f * (zr[k] + cr), ei = 0.5f * (zi[k] + ci);
        float dr = 0.5f * (zr[k] - cr), di = 0.5f * (zi[k] - ci);
        float or_ = di, oi = -dr; // (dr + i*di) / i
        float xr = er + g_post_re[k] * or_ - g_post_im[k] * oi;
        float xi = ei + g_post_re[k] * oi + g_post_im[k] * or_;
        g_mag[k] = sqrtf(xr * xr + xi * xi) * amp_scale;
    }
    g_rate = rate;
}

void spectrum_bands(int count, float *out) {
    const float bin_hz = g_rate / (float)SPECTRUM_FFT_SIZE;
    const unsigned max_bin = SPECTRUM_FFT_SIZE / 2 - 1;

    for (int b = 0; b < count; b++) {
        float lo_hz = BAND_LO_HZ * powf(BAND_HI_HZ / BAND_LO_HZ, (float)b / count);
        float hi_hz = BAND_LO_HZ * powf(BAND_HI_HZ / BAND_LO_HZ, (float)(b + 1) / count);
        unsigned lo = (unsigned)(lo_hz / bin_hz);
        unsigned hi = (unsigned)ceilf(hi_hz / bin_hz);
        if (lo < 1) lo = 1; // bin 0 is DC, not sound
        if (hi > max_bin) hi = max_bin;
        if (hi < lo) hi = lo;

        // Peak, not average, over the band's bins: the low bands span one or
        // two bins and the high ones dozens, and averaging would make a single
        // tone in a wide band look much quieter than the same tone in a narrow
        // one. (Even at 11.7Hz bins the very lowest columns can share a bin
        // and move together -- the FFT's resolution limit, same as in cava.)
        float peak = 0.0f;
        for (unsigned k = lo; k <= hi; k++) {
            if (g_mag[k] > peak) peak = g_mag[k];
        }

        float db = 20.0f * log10f(peak + 1e-9f);
        float center_hz = sqrtf(lo_hz * hi_hz);
        db += TILT_DB_PER_OCTAVE * log2f(center_hz / TILT_PIVOT_HZ);

        float level = (db - FLOOR_DB) / (CEIL_DB - FLOOR_DB);
        out[b] = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
    }
}
