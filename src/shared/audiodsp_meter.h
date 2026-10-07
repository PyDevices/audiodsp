// Band levels for a spectrum meter: N log-spaced bands between two
// frequencies, plus peak and RMS, from interleaved s16 PCM. audiometer's
// engine, shared by the MicroPython module and the CPython extension.
//
// It came from usbif's sound-card pump (the audio meter spike, 2026-09-25),
// and kept its method: two FFTs, because one can't be both quick and sharp at
// the bottom of a log scale.
//
// - 1024 points at the stream's rate (21 ms at 48 kHz, 47 Hz bins) for every
//   band centred above 400 Hz;
// - 512 points on a copy low-passed at 700 Hz (6th-order Butterworth) and
//   decimated by 16 (171 ms, 5.9 Hz bins) for the bands below.
//
// A band's level is the power of the bins it covers, in dB relative to a
// full-scale sine. A band narrower than a bin takes the interpolated bin
// power scaled by its width, so pink noise reads flat across the whole scale.
// Levels go out as one byte each in half-dB steps: 0 is -100 dB or quieter,
// 200 is 0 dB.
//
// What changed in the move is that every number is now the same on every
// interpreter: the window, the filter and the band edges come from
// audiodsp_sincos() and this file's own log and exp rather than libm, and the
// transform is audiodsp_rfft. A meter is eye candy, but its tests compare a
// board with a desktop byte for byte, and three libms don't agree in the last
// place.
//
// Threading: one feeder at a time (the sound-card pump on its own core, or a
// Python thread), and any number of readers. configure() may be called from
// a reader's thread while the feeder runs; the feeder picks it up at its next
// block. Nothing here locks.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIODSP_METER_MAX_BANDS (96)

typedef struct audiodsp_meter audiodsp_meter_t;

// A meter with its buffers (about 40 kB, in pieces of 6 kB or less, so a
// board's allocator keeps them out of slow PSRAM), or NULL when memory ran
// out. Off until configure() asks for bands.
audiodsp_meter_t *audiodsp_meter_new(void);
void audiodsp_meter_free(audiodsp_meter_t *m);

// Asks for `bands` (1..AUDIODSP_METER_MAX_BANDS) log-spaced bands between
// lo_hz and hi_hz, or 0 for off. Takes effect at the next feed.
void audiodsp_meter_configure(audiodsp_meter_t *m, int bands, float lo_hz, float hi_hz);

// Interleaved s16 frames at `rate`. Channels beyond the first two are
// ignored; two are averaged. Runs the analysis inline each time a hop's worth
// of samples (a sixtieth of a second) has arrived.
void audiodsp_meter_feed_s16(audiodsp_meter_t *m, const int16_t *frames,
    uint32_t n, uint32_t channels, uint32_t rate);

// The latest levels, under a sequence lock. Returns the analysis count (0
// before the first), copies min(bands, max) levels and sets *nbands.
uint32_t audiodsp_meter_read(audiodsp_meter_t *m, uint8_t *levels, uint32_t max,
    uint8_t *peak, uint8_t *rms, uint32_t *nbands);

// What it has cost since the last configure(), from the port's clock (zero on
// a port without one).
typedef struct {
    bool enabled;
    uint32_t bands;
    uint32_t analyses;
    uint64_t feed_us;        // the per-block work: mix, filter, rings
    uint64_t analysis_us;    // the FFTs and the bands
    uint32_t max_analysis_us;
    uint64_t elapsed_us;     // since configure()
} audiodsp_meter_stats_t;
void audiodsp_meter_stats(audiodsp_meter_t *m, audiodsp_meter_stats_t *st);

// For the tests: the half-dB byte for a power (1.0 = a full-scale sine).
uint8_t audiodsp_meter_db_byte(float power);
