// audiofeatures' engine; audiodsp_features.h says what it does.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_features.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "shared/audiodsp_microfrontend_names.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/experimental/microfrontend/lib/bits.h"

// --- the resampler -----------------------------------------------------------
//
// A polyphase windowed-sinc filter. The kernel is sinc(u) under a Kaiser
// window (beta 6) that is RS_ZEROS output samples wide on each side, where u
// counts 16 kHz output samples: its cutoff is 8 kHz, it is flat (within
// 0.01 dB) to 7.5 kHz, and from 8.5 kHz up it is at least 62 dB down. What it
// lets through between 8 and 8.5 kHz folds to between 7.5 and 8 kHz, above
// the frontend's top channel, so nothing it aliases reaches a feature.
//
// The ratio is held exactly as L/M (16000/rate in lowest terms), so the
// position of every output sample is exact. When the L rows of taps fit in
// RS_MAX_EXACT floats (every integer ratio, and 24 kHz) they are worked out
// once; otherwise (44.1 kHz and its family) each output's row comes from a
// table of the kernel, linearly interpolated. Each row is scaled to sum to 1,
// so a constant input comes out unchanged.

#define RS_ZEROS 32
#define RS_BETA 6.0
#define RS_TABLE_STEPS 64   // kernel table points per output sample
#define RS_MAX_EXACT 4096
#define RS_BLOCK 256        // input frames taken at a time
#define RS_PI 3.14159265358979323846

typedef struct {
    uint32_t rate, L, M;
    int J0;             // taps before the centre sample
    int T;              // taps in a row
    float *rows;        // L rows of T, or NULL
    float *table;       // RS_ZEROS * RS_TABLE_STEPS + 2 points, or NULL
    float *row;         // the current row, when rows is NULL
    float *buf;         // mono input still needed
    int cap, n;
    int k0;             // the next output's centre is buf[k0] + ph / L
    uint32_t ph;
} rs_t;

static double rs_bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

// sin(pi * x) from + - * / alone. libm's sin() is allowed to differ in the
// last bit between libraries, and one bit in a tap can move a sample, so a
// board's newlib and a desktop's glibc would make different taps. These
// operations are exact IEEE ones everywhere.
static double rs_sinpi(double x) {
    double sign = 1.0;
    if (x < 0.0) {
        x = -x;
        sign = -1.0;
    }
    x -= 2.0 * (double)(long)(x / 2.0);     // now 0 <= x < 2, exactly
    if (x >= 1.0) {
        x -= 1.0;
        sign = -sign;
    }
    if (x > 0.5) {
        x = 1.0 - x;                        // sin(pi x) = sin(pi (1 - x))
    }
    // Taylor series of sin(t), t = pi x <= pi / 2: 13 terms reach 1e-17.
    double t = RS_PI * x, t2 = t * t, term = t, sum = t;
    for (int k = 1; k < 14; k++) {
        term *= -t2 / ((double)(2 * k) * (double)(2 * k + 1));
        sum += term;
    }
    return sign * sum;
}

// The kernel at u output samples from the centre.
static double rs_kernel(double u) {
    u = fabs(u);
    if (u >= RS_ZEROS) {
        return 0.0;
    }
    double r = u / RS_ZEROS;
    double w = rs_bessel_i0(RS_BETA * sqrt(1.0 - r * r)) / rs_bessel_i0(RS_BETA);
    double s = u < 1e-12 ? 1.0 : rs_sinpi(u) / (RS_PI * u);
    return s * w;
}

static uint32_t rs_gcd(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void rs_free(rs_t *rs) {
    free(rs->rows);
    free(rs->table);
    free(rs->row);
    free(rs->buf);
    memset(rs, 0, sizeof(*rs));
}

// u for tap j of phase ph, in output samples.
static double rs_u(const rs_t *rs, uint32_t ph, int j) {
    return ((double)ph + (double)(rs->J0 - j) * rs->L) / rs->M;
}

static void rs_normalize(float *row, int T) {
    double sum = 0.0;
    for (int j = 0; j < T; j++) {
        sum += (double)row[j];
    }
    if (sum != 0.0) {
        float g = (float)(1.0 / sum);
        for (int j = 0; j < T; j++) {
            row[j] *= g;
        }
    }
}

static void rs_restart(rs_t *rs) {
    // History before the first sample is silence, so the first output is
    // centred on the first input sample.
    memset(rs->buf, 0, (size_t)rs->cap * sizeof(float));
    rs->n = rs->J0;
    rs->k0 = rs->J0;
    rs->ph = 0;
}

static bool rs_configure(rs_t *rs, uint32_t rate) {
    rs_free(rs);
    uint32_t g = rs_gcd(AUDIODSP_FEATURES_RATE, rate);
    rs->rate = rate;
    rs->L = AUDIODSP_FEATURES_RATE / g;
    rs->M = rate / g;
    double W = (double)RS_ZEROS * rs->M / rs->L;   // the kernel's half-width in input samples
    rs->J0 = (int)ceil(W);
    rs->T = 2 * rs->J0 + 2;
    rs->cap = rs->T + RS_BLOCK;
    rs->buf = malloc((size_t)rs->cap * sizeof(float));
    if (rs->buf == NULL) {
        goto fail;
    }
    if ((size_t)rs->L * (size_t)rs->T <= RS_MAX_EXACT) {
        rs->rows = malloc((size_t)rs->L * (size_t)rs->T * sizeof(float));
        if (rs->rows == NULL) {
            goto fail;
        }
        for (uint32_t ph = 0; ph < rs->L; ph++) {
            float *row = rs->rows + (size_t)ph * rs->T;
            for (int j = 0; j < rs->T; j++) {
                row[j] = (float)rs_kernel(rs_u(rs, ph, j));
            }
            rs_normalize(row, rs->T);
        }
    } else {
        int pts = RS_ZEROS * RS_TABLE_STEPS + 2;
        rs->table = malloc((size_t)pts * sizeof(float));
        rs->row = malloc((size_t)rs->T * sizeof(float));
        if (rs->table == NULL || rs->row == NULL) {
            goto fail;
        }
        for (int i = 0; i < pts; i++) {
            rs->table[i] = (float)rs_kernel((double)i / RS_TABLE_STEPS);
        }
    }
    rs_restart(rs);
    return true;
fail:
    rs_free(rs);
    return false;
}

static const float *rs_row(rs_t *rs) {
    if (rs->rows != NULL) {
        return rs->rows + (size_t)rs->ph * rs->T;
    }
    float step = (float)rs->L / (float)rs->M * RS_TABLE_STEPS;
    float x = ((float)rs->ph / (float)rs->M + (float)rs->J0 * (float)rs->L / (float)rs->M) * RS_TABLE_STEPS;
    for (int j = 0; j < rs->T; j++, x -= step) {
        float a = fabsf(x);
        int i = (int)a;
        float v = 0.0f;
        if (i < RS_ZEROS * RS_TABLE_STEPS) {
            float fr = a - (float)i;
            v = rs->table[i] + (rs->table[i + 1] - rs->table[i]) * fr;
        }
        rs->row[j] = v;
    }
    rs_normalize(rs->row, rs->T);
    return rs->row;
}

// --- voice activity ----------------------------------------------------------
//
// Each slice gets a flag saying whether it holds speech, worked out from the
// filterbank's own band amplitudes, before noise reduction touches them.
// Everything is integer arithmetic, so every target flags the same slices.
//
// Per band, the level is log2 of the amplitude in 1/64 octave (0.094 dB) steps,
// and a noise floor follows it: down 2 steps a slice when the level is under
// it, up 1/4 step a slice when it is over. That holds the floor near the
// quietest tenth of what the band hears, so speech, which comes and goes,
// barely moves it, and a steady noise becomes the floor in a few seconds. A
// band quieter than any real microphone (digital silence between files, say)
// leaves its floor alone.
//
// The floor starts as the mean of the first VAD_PRIME slices, which are
// never voice. A slice's score is the mean, over the VAD_TOP bands furthest above their
// floors, of how far above they are, averaged over the last VAD_SMOOTH
// slices. Voice is a score of VAD_ON or more, and it holds for VAD_HOLD slices
// after the score drops, so a word's tail and the gaps between words stay in.
// While the flag is up the floor rises more slowly, and then faster the longer
// it stays up, so a noise that steps up and stays is taken in within a few
// seconds instead of reading as speech forever.

#define VAD_TOP 4               // bands scored
#define VAD_ON 110              // score for speech: about 10 dB above the floor
#define VAD_CLIP 192            // a band counts at most 18 dB above its floor
#define VAD_SMOOTH 5            // slices the score is averaged over
#define VAD_HOLD 10             // slices voice holds after the score drops
#define VAD_LEVEL_MIN 200       // a level under this (about -80 dBFS) is no news
#define VAD_FALL 32             // floor steps, in 1/16 of a level step a slice
#define VAD_RISE 4
#define VAD_RISE_VOICE 2
#define VAD_RISE_SHIFT 4        // the rise grows by one every 2^4 slices of voice
#define VAD_PRIME 16            // slices whose mean starts the floor, never voice

typedef struct {
    int32_t floor[AUDIODSP_FEATURES_SIZE];  // each band's noise floor, 1/1024 octave
    int16_t recent[VAD_SMOOTH];             // the last slices' scores
    uint16_t run;                           // slices voiced in a row
    uint8_t at, hold, voice;
    uint8_t primed;                         // slices heard, up to VAD_PRIME
} audiodsp_vad_t;

// log2(x) in 1/64 octave: the top bit's place, then six bits of mantissa,
// read as linear. 0 and 1 both give 0.
static int32_t vad_level(uint32_t x) {
    if (x < 2) {
        return 0;
    }
    int m = MostSignificantBit32(x) - 1;
    uint32_t frac = m >= 6 ? (x >> (m - 6)) & 63 : (x << (6 - m)) & 63;
    return m * 64 + (int32_t)frac;
}

static void vad_reset(audiodsp_vad_t *v) {
    memset(v, 0, sizeof(*v));
}

static uint8_t vad_slice(audiodsp_vad_t *v, const uint32_t *amp) {
    int32_t level[AUDIODSP_FEATURES_SIZE], above[AUDIODSP_FEATURES_SIZE];
    for (int b = 0; b < AUDIODSP_FEATURES_SIZE; b++) {
        level[b] = vad_level(amp[b]);
    }
    if (v->primed < VAD_PRIME) {
        // The floor starts as the mean of the first slices: one slice alone
        // could catch a band low and read the next ones as voice.
        int32_t n = v->primed;
        for (int b = 0; b < AUDIODSP_FEATURES_SIZE; b++) {
            int32_t l = level[b] < VAD_LEVEL_MIN ? VAD_LEVEL_MIN : level[b];
            v->floor[b] = (v->floor[b] * n + (l << 4)) / (n + 1);
        }
        v->primed++;
        v->voice = 0;
        return 0;
    }
    for (int b = 0; b < AUDIODSP_FEATURES_SIZE; b++) {
        int32_t d = level[b] - (v->floor[b] >> 4);
        above[b] = d < 0 ? 0 : d > VAD_CLIP ? VAD_CLIP : d;
    }
    // The mean of the VAD_TOP largest, by selection.
    int32_t sum = 0;
    for (int i = 0; i < VAD_TOP; i++) {
        int best = i;
        for (int j = i + 1; j < AUDIODSP_FEATURES_SIZE; j++) {
            if (above[j] > above[best]) {
                best = j;
            }
        }
        int32_t t = above[i];
        above[i] = above[best];
        above[best] = t;
        sum += above[i];
    }
    v->recent[v->at] = (int16_t)(sum / VAD_TOP);
    v->at = (uint8_t)((v->at + 1) % VAD_SMOOTH);
    int32_t score = 0;
    for (int i = 0; i < VAD_SMOOTH; i++) {
        score += v->recent[i];
    }
    score /= VAD_SMOOTH;
    uint8_t voice;
    if (score >= VAD_ON) {
        v->hold = VAD_HOLD;
        voice = 1;
    } else if (v->hold > 0) {
        v->hold--;
        voice = 1;
    } else {
        voice = 0;
    }
    int32_t rise = voice ? VAD_RISE_VOICE + (int32_t)(v->run >> VAD_RISE_SHIFT) : VAD_RISE;
    for (int b = 0; b < AUDIODSP_FEATURES_SIZE; b++) {
        if (level[b] < VAD_LEVEL_MIN) {
            continue;
        }
        int32_t t = level[b] << 4;
        int32_t d = t - v->floor[b];
        if (d < 0) {
            v->floor[b] -= -d < VAD_FALL ? -d : VAD_FALL;
        } else {
            v->floor[b] += d < rise ? d : rise;
        }
    }
    v->run = voice ? (v->run < 65535 ? v->run + 1 : v->run) : 0;
    v->voice = voice;
    return voice;
}

// --- the engine --------------------------------------------------------------

struct audiodsp_features {
    struct FrontendConfig config;
    struct FrontendState fe;
    bool fe_ready;
    rs_t rs;
    uint32_t rate;          // last rate fed
    uint16_t *queue;        // capacity slices
    uint8_t *voice;         // a flag for each queued slice
    audiodsp_vad_t vad;
    uint32_t capacity, head, count;
    uint32_t slices, dropped;
};

int8_t audiodsp_features_quantize(uint16_t value) {
    int32_t v = (((int32_t)value * 256) + 333) / 666 - 128;
    return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

static void features_config(struct FrontendConfig *c) {
    // ESPHome's micro_wake_word settings, which microWakeWord's models were
    // trained with.
    FrontendFillConfigWithDefaults(c);
    c->window.size_ms = AUDIODSP_FEATURES_WINDOW_MS;
    c->window.step_size_ms = AUDIODSP_FEATURES_STEP_MS;
    c->filterbank.num_channels = AUDIODSP_FEATURES_SIZE;
    c->filterbank.lower_band_limit = 125.0f;
    c->filterbank.upper_band_limit = 7500.0f;
    c->noise_reduction.smoothing_bits = 10;
    c->noise_reduction.even_smoothing = 0.025f;
    c->noise_reduction.odd_smoothing = 0.06f;
    c->noise_reduction.min_signal_remaining = 0.05f;
    c->pcan_gain_control.enable_pcan = 1;
    c->pcan_gain_control.strength = 0.95f;
    c->pcan_gain_control.offset = 80.0f;
    c->pcan_gain_control.gain_bits = 21;
    c->log_scale.enable_log = 1;
    c->log_scale.scale_shift = 6;
}

audiodsp_features_t *audiodsp_features_new(uint32_t capacity) {
    if (capacity == 0) {
        capacity = 1;
    }
    audiodsp_features_t *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->capacity = capacity;
    f->queue = malloc((size_t)capacity * AUDIODSP_FEATURES_SIZE * sizeof(uint16_t));
    f->voice = malloc(capacity);
    features_config(&f->config);
    if (f->queue == NULL || f->voice == NULL) {
        audiodsp_features_free(f);
        return NULL;
    }
    memset(&f->fe, 0, sizeof(f->fe));
    if (!FrontendPopulateState(&f->config, &f->fe, AUDIODSP_FEATURES_RATE)) {
        // What it allocated before failing is freed with it.
        f->fe_ready = true;
        audiodsp_features_free(f);
        return NULL;
    }
    f->fe_ready = true;
    return f;
}

void audiodsp_features_free(audiodsp_features_t *f) {
    if (f == NULL) {
        return;
    }
    if (f->fe_ready) {
        FrontendFreeStateContents(&f->fe);
    }
    rs_free(&f->rs);
    free(f->queue);
    free(f->voice);
    free(f);
}

void audiodsp_features_reset(audiodsp_features_t *f) {
    FrontendReset(&f->fe);
    if (f->rs.buf != NULL) {
        rs_restart(&f->rs);
    }
    vad_reset(&f->vad);
    f->head = f->count = 0;
    f->slices = f->dropped = 0;
}

static void features_push(audiodsp_features_t *f, const uint16_t *values, uint8_t voice) {
    if (f->count == f->capacity) {
        f->head = (f->head + 1) % f->capacity;
        f->count--;
        f->dropped++;
    }
    uint32_t tail = (f->head + f->count) % f->capacity;
    memcpy(f->queue + (size_t)tail * AUDIODSP_FEATURES_SIZE, values, AUDIODSP_FEATURES_SIZE * sizeof(uint16_t));
    f->voice[tail] = voice;
    f->count++;
    f->slices++;
}

// One slice, when the window has filled: FrontendProcessSamples() step for
// step (frontend.c), so the values are the library's own, with a copy of the
// band amplitudes taken for voice activity before noise reduction changes
// them. Returns false while the window still wants samples.
static bool features_slice(audiodsp_features_t *f, const int16_t *x, size_t n, size_t *read) {
    struct FrontendState *st = &f->fe;
    if (!WindowProcessSamples(&st->window, x, n, read)) {
        return false;
    }
    int input_shift = 15 - MostSignificantBit32(st->window.max_abs_output_value);
    FftCompute(&st->fft, st->window.output, input_shift);
    int32_t *energy = (int32_t *)st->fft.output;
    FilterbankConvertFftComplexToEnergy(&st->filterbank, st->fft.output, energy);
    FilterbankAccumulateChannels(&st->filterbank, energy);
    uint32_t *scaled = FilterbankSqrt(&st->filterbank, input_shift);
    uint8_t voice = vad_slice(&f->vad, scaled);
    NoiseReductionApply(&st->noise_reduction, scaled);
    if (st->pcan_gain_control.enable_pcan) {
        PcanGainControlApply(&st->pcan_gain_control, scaled);
    }
    int correction_bits = MostSignificantBit32(st->fft.fft_size) - 1 - (kFilterbankBits / 2);
    uint16_t *logged = LogScaleApply(&st->log_scale, scaled, st->filterbank.num_channels, correction_bits);
    features_push(f, logged, voice);
    return true;
}

// 16 kHz mono samples through the frontend.
static void features_run(audiodsp_features_t *f, const int16_t *x, size_t n) {
    while (n > 0) {
        size_t read = 0;
        bool made = features_slice(f, x, n, &read);
        if (read == 0 && !made) {
            break;
        }
        x += read;
        n -= read;
    }
}

static int16_t features_round(float y) {
    if (y >= 32767.0f) {
        return 32767;
    }
    if (y <= -32768.0f) {
        return -32768;
    }
    return (int16_t)(y >= 0.0f ? (int32_t)(y + 0.5f) : -(int32_t)(0.5f - y));
}

int audiodsp_features_feed_s16(audiodsp_features_t *f, const int16_t *pcm, size_t frames,
    uint32_t channels, uint32_t rate) {
    if (rate < AUDIODSP_FEATURES_MIN_RATE || rate > AUDIODSP_FEATURES_MAX_RATE || channels == 0) {
        return -1;
    }
    if (rate != f->rate) {
        if (rate == AUDIODSP_FEATURES_RATE) {
            rs_free(&f->rs);
        } else if (!rs_configure(&f->rs, rate)) {
            f->rate = 0;
            return -1;
        }
        f->rate = rate;
    }
    int16_t mono[RS_BLOCK];
    if (rate == AUDIODSP_FEATURES_RATE) {
        if (channels == 1) {
            features_run(f, pcm, frames);
            return 0;
        }
        while (frames > 0) {
            size_t m = frames < RS_BLOCK ? frames : RS_BLOCK;
            for (size_t i = 0; i < m; i++) {
                int32_t s = 0;
                for (uint32_t c = 0; c < channels; c++) {
                    s += pcm[i * channels + c];
                }
                int32_t h = (int32_t)channels / 2;
                mono[i] = (int16_t)((s >= 0 ? s + h : s - h) / (int32_t)channels);
            }
            features_run(f, mono, m);
            pcm += m * channels;
            frames -= m;
        }
        return 0;
    }
    rs_t *rs = &f->rs;
    float scale = 1.0f / (float)channels;
    while (frames > 0) {
        int room = rs->cap - rs->n;
        size_t m = frames < (size_t)room ? frames : (size_t)room;
        float *dst = rs->buf + rs->n;
        for (size_t i = 0; i < m; i++) {
            int32_t s = 0;
            for (uint32_t c = 0; c < channels; c++) {
                s += pcm[i * channels + c];
            }
            dst[i] = (float)s * scale;
        }
        rs->n += (int)m;
        pcm += m * channels;
        frames -= m;
        // Every output whose taps are all here.
        size_t out = 0;
        while (rs->k0 - rs->J0 + rs->T <= rs->n) {
            const float *row = rs_row(rs);
            const float *x = rs->buf + rs->k0 - rs->J0;
            float y = 0.0f;
            for (int j = 0; j < rs->T; j++) {
                y += row[j] * x[j];
            }
            mono[out++] = features_round(y);
            if (out == RS_BLOCK) {
                features_run(f, mono, out);
                out = 0;
            }
            rs->ph += rs->M;
            rs->k0 += (int)(rs->ph / rs->L);
            rs->ph %= rs->L;
        }
        features_run(f, mono, out);
        // Keep only what the next output needs.
        int drop = rs->k0 - rs->J0;
        if (drop > 0) {
            if (drop > rs->n) {
                drop = rs->n;
            }
            memmove(rs->buf, rs->buf + drop, (size_t)(rs->n - drop) * sizeof(float));
            rs->n -= drop;
            rs->k0 -= drop;
        }
    }
    return 0;
}

uint32_t audiodsp_features_room(const audiodsp_features_t *f) {
    return f->capacity - f->count;
}

uint32_t audiodsp_features_waiting(const audiodsp_features_t *f) {
    return f->count;
}

size_t audiodsp_features_read_u16(audiodsp_features_t *f, uint16_t *out, size_t max_slices, uint8_t *voice) {
    size_t n = 0;
    while (n < max_slices && f->count > 0) {
        if (voice != NULL) {
            voice[n] = f->voice[f->head];
        }
        memcpy(out + n * AUDIODSP_FEATURES_SIZE, f->queue + (size_t)f->head * AUDIODSP_FEATURES_SIZE,
            AUDIODSP_FEATURES_SIZE * sizeof(uint16_t));
        f->head = (f->head + 1) % f->capacity;
        f->count--;
        n++;
    }
    return n;
}

size_t audiodsp_features_read_s8(audiodsp_features_t *f, int8_t *out, size_t max_slices, uint8_t *voice) {
    size_t n = 0;
    while (n < max_slices && f->count > 0) {
        if (voice != NULL) {
            voice[n] = f->voice[f->head];
        }
        const uint16_t *v = f->queue + (size_t)f->head * AUDIODSP_FEATURES_SIZE;
        for (int i = 0; i < AUDIODSP_FEATURES_SIZE; i++) {
            out[n * AUDIODSP_FEATURES_SIZE + i] = audiodsp_features_quantize(v[i]);
        }
        f->head = (f->head + 1) % f->capacity;
        f->count--;
        n++;
    }
    return n;
}

bool audiodsp_features_speaking(const audiodsp_features_t *f) {
    return f->vad.voice != 0;
}

void audiodsp_features_stats(const audiodsp_features_t *f, audiodsp_features_stats_t *st) {
    st->slices = f->slices;
    st->dropped = f->dropped;
    st->waiting = f->count;
    st->capacity = f->capacity;
    st->rate = f->rate;
    st->resampling = f->rate != 0 && f->rate != AUDIODSP_FEATURES_RATE;
}
