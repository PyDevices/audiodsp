// audiometer's engine. See audiodsp_meter.h for the method.
//
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "shared/audiodsp_fft.h"
#include "shared/audiodsp_hot.h"
#include "shared/audiodsp_meter.h"
#include "shared/audiodsp_port.h"
#include "shared/audiodsp_trig.h"

#if defined(__GNUC__) || defined(__clang__)
#define METER_LOAD_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define METER_STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define METER_FENCE()         __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
// MSVC: the CPython wheel, where one thread feeds and reads.
#define METER_LOAD_ACQ(p)     (*(volatile uint32_t *)(p))
#define METER_STORE_REL(p, v) (*(volatile uint32_t *)(p) = (v))
#define METER_FENCE()         ((void)0)
#endif

#define HI_N (1024)
#define LO_N (512)
#define LO_DEC (16)
#define METER_HZ (60)
#define LP_HZ (700.0)
#define SPLIT_HZ (400.0)
#define NSEC (3)

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

// One band: which FFT, and the bins it covers. Bin k spans k - 1/2 to
// k + 1/2; the band takes all of bins k0+1 .. k1-1 and the part of k0 and k1
// that lies inside it, so its power is the spectrum integrated over the band.
// (Whole bins only, as the spike had it, put a band one to three bins wide up
// to 4 dB out on pink noise.)
typedef struct {
    uint8_t lo;       // 1: the decimated FFT
    uint16_t k0, k1;  // first and last bin, k0 <= k1
    float w0, w1;     // the share of bin k0 and of bin k1 (one bin: w0 alone)
} band_t;

struct audiodsp_meter {
    // Asked for by configure(); picked up by the feeder at its next block.
    uint32_t want_config;
    uint8_t want_bands;
    float want_lo_hz, want_hi_hz;

    bool enabled;
    uint8_t nbands;
    float lo_hz, hi_hz;
    uint32_t rate;

    float *hi_ring, *lo_ring;
    uint32_t hi_pos, lo_pos;
    float *win_hi, *win_lo;
    float *lin, *scratch, *spec, *pw;
    float *tab_hi, *tab_lo;
    audiodsp_rfft_t fft_hi, fft_lo;
    float norm_hi, norm_lo;
    biquad_t lp[NSEC];
    uint32_t dec_phase;
    uint32_t hop, hop_count;
    float peak, sumsq;
    uint32_t nsum;
    band_t band[AUDIODSP_METER_MAX_BANDS];
    uint8_t out[AUDIODSP_METER_MAX_BANDS];

    // Published: a sequence count (odd while writing) and the levels.
    uint32_t seq;
    uint8_t levels[AUDIODSP_METER_MAX_BANDS];
    uint8_t peak_b, rms_b;

    // Cost, from the port's clock.
    uint64_t feed_us, analysis_us, t0;
    uint32_t analyses, max_analysis_us;
};

// --- log and exp, the same on every interpreter ---------------------------
//
// libm's agree to an ulp and differ in the last place, and a band edge or a
// half-dB rounding sits on whatever that place says. These are frexp/ldexp
// (exact everywhere) around short series evaluated in a fixed order.

static double meter_ln(double x) {
    int e;
    double m = frexp(x, &e);        // x = m * 2^e, m in [0.5, 1)
    if (m < 0.70710678118654752) {
        m *= 2.0;
        e -= 1;
    }
    // ln m = 2 atanh(t), |t| <= 0.172: eight odd terms reach 1e-13
    const double t = (m - 1.0) / (m + 1.0), t2 = t * t;
    double sum = 0.0, p = t;
    for (int k = 1; k <= 15; k += 2) {
        sum += p / k;
        p *= t2;
    }
    return 2.0 * sum + e * 0.69314718055994530942;
}

static double meter_exp(double y) {
    const double ln2 = 0.69314718055994530942;
    const int k = (int)floor(y / ln2 + 0.5);
    const double f = y - k * ln2;     // |f| <= 0.347
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 14; i++) {
        term *= f / i;
        sum += term;
    }
    return ldexp(sum, k);
}

uint8_t audiodsp_meter_db_byte(float power) {
    if (power <= 1e-10f) {
        return 0;
    }
    // half-dB steps: 2 * (10 log10 p + 100)
    const double v = 20.0 * meter_ln((double)power) / 2.30258509299404568402 + 200.0;
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)(v + 0.5);
}

// --- set-up -----------------------------------------------------------------

static uint64_t meter_now(void) {
    const audiodsp_port_ops_t *port = audiodsp_port();
    return port->now_us ? port->now_us() : 0;
}

static void meter_hann(float *w, int n, float *norm) {
    double s2 = 0;
    for (int i = 0; i < n; i++) {
        audiodsp_sincos_t sc;
        audiodsp_sincos(2.0 * AUDIODSP_PI * i / n, &sc);
        w[i] = (float)(0.5 - 0.5 * sc.c);
        s2 += (double)w[i] * (double)w[i];
    }
    *norm = (float)(2.0 / (n * s2));
}

audiodsp_meter_t *audiodsp_meter_new(void) {
    audiodsp_meter_t *m = calloc(1, sizeof(audiodsp_meter_t));
    if (m == NULL) {
        return NULL;
    }
    m->hi_ring = calloc(HI_N, sizeof(float));
    m->lo_ring = calloc(LO_N, sizeof(float));
    m->win_hi = malloc(HI_N * sizeof(float));
    m->win_lo = malloc(LO_N * sizeof(float));
    m->lin = malloc(HI_N * sizeof(float));
    m->scratch = malloc(HI_N * sizeof(float));
    m->spec = malloc((HI_N + 2) * sizeof(float));
    m->pw = malloc(HI_N / 2 * sizeof(float));
    m->tab_hi = malloc(audiodsp_rfft_table_floats(HI_N) * sizeof(float));
    m->tab_lo = malloc(audiodsp_rfft_table_floats(LO_N) * sizeof(float));
    if (!m->hi_ring || !m->lo_ring || !m->win_hi || !m->win_lo || !m->lin || !m->scratch
        || !m->spec || !m->pw || !m->tab_hi || !m->tab_lo) {
        audiodsp_meter_free(m);
        return NULL;
    }
    meter_hann(m->win_hi, HI_N, &m->norm_hi);
    meter_hann(m->win_lo, LO_N, &m->norm_lo);
    audiodsp_rfft_init(&m->fft_hi, HI_N, m->tab_hi);
    audiodsp_rfft_init(&m->fft_lo, LO_N, m->tab_lo);
    return m;
}

void audiodsp_meter_free(audiodsp_meter_t *m) {
    if (m == NULL) {
        return;
    }
    free(m->hi_ring), free(m->lo_ring), free(m->win_hi), free(m->win_lo), free(m->lin);
    free(m->scratch), free(m->spec), free(m->pw), free(m->tab_hi), free(m->tab_lo);
    free(m);
}

void audiodsp_meter_configure(audiodsp_meter_t *m, int bands, float lo_hz, float hi_hz) {
    if (bands > AUDIODSP_METER_MAX_BANDS) {
        bands = AUDIODSP_METER_MAX_BANDS;
    }
    m->want_bands = (uint8_t)(bands < 0 ? 0 : bands);
    m->want_lo_hz = lo_hz;
    m->want_hi_hz = hi_hz;
    METER_STORE_REL(&m->want_config, 1);
}

// Butterworth low-pass sections (RBJ biquads at the Butterworth Qs), the
// hop, and each band's bins.
static void meter_design(audiodsp_meter_t *m, uint32_t rate) {
    static const double q[NSEC] = { 0.51764, 0.70711, 1.93185 };
    audiodsp_sincos_t sc;
    audiodsp_sincos(2.0 * AUDIODSP_PI * LP_HZ / rate, &sc);
    for (int i = 0; i < NSEC; i++) {
        const double alpha = sc.s / (2.0 * q[i]);
        const double a0 = 1.0 + alpha;
        biquad_t *b = &m->lp[i];
        b->b0 = (float)((1.0 - sc.c) / 2.0 / a0);
        b->b1 = (float)((1.0 - sc.c) / a0);
        b->b2 = b->b0;
        b->a1 = (float)(-2.0 * sc.c / a0);
        b->a2 = (float)((1.0 - alpha) / a0);
        b->z1 = b->z2 = 0;
    }
    m->hop = rate / METER_HZ;
    m->hop_count = 0;
    m->dec_phase = 0;

    const double r = meter_exp(meter_ln((double)m->hi_hz / (double)m->lo_hz) / m->nbands);
    const double lo_rate = (double)rate / LO_DEC;
    double f0 = m->lo_hz;
    for (int i = 0; i < m->nbands; i++, f0 *= r) {
        const double f1 = f0 * r;
        const double fc = sqrt(f0 * f1);
        band_t *b = &m->band[i];
        b->lo = (fc < SPLIT_HZ);
        const double w = b->lo ? lo_rate / LO_N : (double)rate / HI_N;
        const int last = (b->lo ? LO_N : HI_N) / 2 - 1;    // pw[] holds bins 0 .. n/2 - 1
        double a = f0 / w, z = f1 / w;
        if (z > last + 0.5) {
            z = last + 0.5;
        }
        if (a < 0.5) {
            a = 0.5;                            // bin 0 is DC, and pw[0] is 0
        }
        if (a > z) {
            a = z;
        }
        int k0 = (int)floor(a + 0.5), k1 = (int)floor(z + 0.5);
        k0 = k0 > last ? last : k0;
        k1 = k1 > last ? last : k1;
        b->k0 = (uint16_t)k0;
        b->k1 = (uint16_t)k1;
        if (k0 == k1) {
            b->w0 = (float)(z - a);
            b->w1 = 0;
        } else {
            b->w0 = (float)(k0 + 0.5 - a);
            b->w1 = (float)(z - (k1 - 0.5));
        }
    }
}

// --- the analysis -----------------------------------------------------------

// The ring, oldest first, windowed, into lin; transformed; |X[k]|^2 into pw.
static void AUDIODSP_HOT meter_power(audiodsp_meter_t *m, const float *ring, uint32_t pos,
    const float *win, const audiodsp_rfft_t *fft, int n) {
    const uint32_t mask = (uint32_t)n - 1;
    float *lin = m->lin;
    for (int i = 0; i < n; i++) {
        lin[i] = ring[(pos + i) & mask] * win[i];
    }
    audiodsp_rfft_forward(fft, lin, m->spec, m->scratch);
    const float *s = m->spec;
    float *pw = m->pw;
    pw[0] = 0;
    for (int k = 1; k < n / 2; k++) {
        pw[k] = s[2 * k] * s[2 * k] + s[2 * k + 1] * s[2 * k + 1];
    }
}

static float AUDIODSP_HOT band_power(const audiodsp_meter_t *m, const band_t *b, float norm) {
    const float *pw = m->pw;
    float s = pw[b->k0] * b->w0;
    if (b->k1 > b->k0) {
        for (int k = b->k0 + 1; k < b->k1; k++) {
            s += pw[k];
        }
        s += pw[b->k1] * b->w1;
    }
    return s * norm;
}

static void AUDIODSP_HOT meter_analyse(audiodsp_meter_t *m) {
    uint8_t *out = m->out;
    // The fast FFT for the upper bands.
    meter_power(m, m->hi_ring, m->hi_pos, m->win_hi, &m->fft_hi, HI_N);
    for (int i = 0; i < m->nbands; i++) {
        if (!m->band[i].lo) {
            out[i] = audiodsp_meter_db_byte(band_power(m, &m->band[i], m->norm_hi) * 2.0f);
        }
    }
    // The slow, sharp one for the bottom. Its window is 171 ms long, so
    // every other hop is plenty.
    if (m->band[0].lo && (m->analyses & 1) == 0) {
        meter_power(m, m->lo_ring, m->lo_pos, m->win_lo, &m->fft_lo, LO_N);
        for (int i = 0; i < m->nbands; i++) {
            if (m->band[i].lo) {
                out[i] = audiodsp_meter_db_byte(band_power(m, &m->band[i], m->norm_lo) * 2.0f);
            }
        }
    }
    const uint8_t pk = audiodsp_meter_db_byte(m->peak * m->peak);
    const uint8_t rms = audiodsp_meter_db_byte(m->nsum ? m->sumsq / m->nsum : 0);
    m->peak = 0;
    m->sumsq = 0;
    m->nsum = 0;

    METER_STORE_REL(&m->seq, m->seq + 1);
    METER_FENCE();
    memcpy(m->levels, out, m->nbands);
    m->peak_b = pk;
    m->rms_b = rms;
    METER_FENCE();
    METER_STORE_REL(&m->seq, m->seq + 1);
}

void AUDIODSP_HOT audiodsp_meter_feed_s16(audiodsp_meter_t *m, const int16_t *frames,
    uint32_t n, uint32_t channels, uint32_t rate) {
    if (METER_LOAD_ACQ(&m->want_config)) {
        METER_STORE_REL(&m->want_config, 0);
        m->nbands = m->want_bands;
        m->lo_hz = m->want_lo_hz;
        m->hi_hz = m->want_hi_hz;
        m->enabled = m->nbands > 0;
        m->rate = 0;  // redesign below
        memset(m->out, 0, sizeof(m->out));
        m->feed_us = m->analysis_us = 0;
        m->analyses = m->max_analysis_us = 0;
        m->t0 = meter_now();
    }
    if (!m->enabled || n == 0 || rate == 0 || channels == 0) {
        return;
    }
    const uint64_t t0 = meter_now();
    if (rate != m->rate) {
        m->rate = rate;
        meter_design(m, rate);
    }
    const float k = 1.0f / 32768.0f;
    float pk = m->peak, ss = m->sumsq;
    uint32_t ns = m->nsum, hp = m->hi_pos, lp = m->lo_pos, ph = m->dec_phase;
    float *hr = m->hi_ring, *lr = m->lo_ring;
    // The three low-pass sections, held in registers for the block.
    biquad_t q0 = m->lp[0], q1 = m->lp[1], q2 = m->lp[2];
    for (uint32_t f = 0; f < n; f++) {
        float x;
        if (channels >= 2) {
            x = ((float)frames[channels * f] + (float)frames[channels * f + 1]) * (0.5f * k);
        } else {
            x = (float)frames[f] * k;
        }
        const float ax = fabsf(x);
        if (ax > pk) {
            pk = ax;
        }
        ss += x * x;
        ns++;
        hr[hp] = x;
        hp = (hp + 1) & (HI_N - 1);
        // Low-pass, then keep one sample in LO_DEC.
        float o = q0.b0 * x + q0.z1;
        q0.z1 = q0.b1 * x - q0.a1 * o + q0.z2;
        q0.z2 = q0.b2 * x - q0.a2 * o;
        float y = o;
        o = q1.b0 * y + q1.z1;
        q1.z1 = q1.b1 * y - q1.a1 * o + q1.z2;
        q1.z2 = q1.b2 * y - q1.a2 * o;
        y = o;
        o = q2.b0 * y + q2.z1;
        q2.z1 = q2.b1 * y - q2.a1 * o + q2.z2;
        q2.z2 = q2.b2 * y - q2.a2 * o;
        if (++ph == LO_DEC) {
            ph = 0;
            lr[lp] = o;
            lp = (lp + 1) & (LO_N - 1);
        }
        // A hop ends mid-block as often as not: analyse there, so the levels
        // describe the same samples however the stream is cut into blocks.
        if (++m->hop_count == m->hop) {
            m->hop_count = 0;
            m->hi_pos = hp;
            m->lo_pos = lp;
            m->peak = pk;
            m->sumsq = ss;
            m->nsum = ns;
            const uint64_t a0 = meter_now();
            meter_analyse(m);
            const uint64_t d = meter_now() - a0;
            m->analysis_us += d;
            m->analyses++;
            if (d > m->max_analysis_us) {
                m->max_analysis_us = (uint32_t)d;
            }
            pk = 0;
            ss = 0;
            ns = 0;
        }
    }
    m->lp[0] = q0;
    m->lp[1] = q1;
    m->lp[2] = q2;
    m->peak = pk;
    m->sumsq = ss;
    m->nsum = ns;
    m->hi_pos = hp;
    m->lo_pos = lp;
    m->dec_phase = ph;
    m->feed_us += meter_now() - t0;
}

uint32_t audiodsp_meter_read(audiodsp_meter_t *m, uint8_t *levels, uint32_t max,
    uint8_t *peak, uint8_t *rms, uint32_t *nbands) {
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t s = METER_LOAD_ACQ(&m->seq);
        if (s & 1) {
            continue;
        }
        METER_FENCE();
        const uint32_t n = m->nbands < max ? m->nbands : max;
        memcpy(levels, m->levels, n);
        *peak = m->peak_b;
        *rms = m->rms_b;
        *nbands = n;
        METER_FENCE();
        if (METER_LOAD_ACQ(&m->seq) == s) {
            return s / 2;
        }
    }
    *nbands = 0;
    return 0;
}

void audiodsp_meter_stats(audiodsp_meter_t *m, audiodsp_meter_stats_t *st) {
    st->enabled = m->enabled;
    st->bands = m->nbands;
    st->analyses = m->analyses;
    st->analysis_us = m->analysis_us;
    st->max_analysis_us = m->max_analysis_us;
    // feed_us times the whole block, analyses included: report the feed alone
    st->feed_us = m->feed_us > m->analysis_us ? m->feed_us - m->analysis_us : 0;
    st->elapsed_us = m->enabled && m->t0 ? meter_now() - m->t0 : 0;
}
