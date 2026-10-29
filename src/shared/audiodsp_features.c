// audiofeatures' engine; audiodsp_features.h says what it does.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include "shared/audiodsp_features.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "shared/audiodsp_microfrontend_names.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

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

// The kernel at u output samples from the centre.
static double rs_kernel(double u) {
    u = fabs(u);
    if (u >= RS_ZEROS) {
        return 0.0;
    }
    double r = u / RS_ZEROS;
    double w = rs_bessel_i0(RS_BETA * sqrt(1.0 - r * r)) / rs_bessel_i0(RS_BETA);
    double s = u < 1e-12 ? 1.0 : sin(RS_PI * u) / (RS_PI * u);
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

// --- the engine --------------------------------------------------------------

struct audiodsp_features {
    struct FrontendConfig config;
    struct FrontendState fe;
    bool fe_ready;
    rs_t rs;
    uint32_t rate;          // last rate fed
    uint16_t *queue;        // capacity slices
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
    features_config(&f->config);
    if (f->queue == NULL) {
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
    free(f);
}

void audiodsp_features_reset(audiodsp_features_t *f) {
    FrontendReset(&f->fe);
    if (f->rs.buf != NULL) {
        rs_restart(&f->rs);
    }
    f->head = f->count = 0;
    f->slices = f->dropped = 0;
}

static void features_push(audiodsp_features_t *f, const uint16_t *values) {
    if (f->count == f->capacity) {
        f->head = (f->head + 1) % f->capacity;
        f->count--;
        f->dropped++;
    }
    uint32_t tail = (f->head + f->count) % f->capacity;
    memcpy(f->queue + (size_t)tail * AUDIODSP_FEATURES_SIZE, values, AUDIODSP_FEATURES_SIZE * sizeof(uint16_t));
    f->count++;
    f->slices++;
}

// 16 kHz mono samples through the frontend.
static void features_run(audiodsp_features_t *f, const int16_t *x, size_t n) {
    while (n > 0) {
        size_t read = 0;
        struct FrontendOutput out = FrontendProcessSamples(&f->fe, x, n, &read);
        if (read == 0) {
            break;
        }
        x += read;
        n -= read;
        if (out.size == AUDIODSP_FEATURES_SIZE) {
            features_push(f, out.values);
        }
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

size_t audiodsp_features_read_u16(audiodsp_features_t *f, uint16_t *out, size_t max_slices) {
    size_t n = 0;
    while (n < max_slices && f->count > 0) {
        memcpy(out + n * AUDIODSP_FEATURES_SIZE, f->queue + (size_t)f->head * AUDIODSP_FEATURES_SIZE,
            AUDIODSP_FEATURES_SIZE * sizeof(uint16_t));
        f->head = (f->head + 1) % f->capacity;
        f->count--;
        n++;
    }
    return n;
}

size_t audiodsp_features_read_s8(audiodsp_features_t *f, int8_t *out, size_t max_slices) {
    size_t n = 0;
    while (n < max_slices && f->count > 0) {
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

void audiodsp_features_stats(const audiodsp_features_t *f, audiodsp_features_stats_t *st) {
    st->slices = f->slices;
    st->dropped = f->dropped;
    st->waiting = f->count;
    st->capacity = f->capacity;
    st->rate = f->rate;
    st->resampling = f->rate != 0 && f->rate != AUDIODSP_FEATURES_RATE;
}
