// Six-operator FM synthesis. See audiodsp_fm.h for what this is, where it
// came from and what changed.
//
// Copyright 2012 Google Inc.
// Copyright 2026 Brad Barnett
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "shared/audiodsp_fm.h"

#include <string.h>

#include "shared/audiodsp_fm_tables.h"

#define FM_N AUDIODSP_FM_N
#define FM_LG_N AUDIODSP_FM_LG_N

// An operator whose gain stays under this for a whole control block is not
// computed: its output would be under a tenth of an LSB.
#define FM_LEVEL_THRESHOLD 1120

#define FM_OP_MAX 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 3, 3, 7, 3, 7, \
    99, 1, 31, 99, 14

const uint8_t audiodsp_fm_patch_max[AUDIODSP_FM_PATCH_BYTES] = {
    FM_OP_MAX, FM_OP_MAX, FM_OP_MAX, FM_OP_MAX, FM_OP_MAX, FM_OP_MAX,
    99, 99, 99, 99, 99, 99, 99, 99,     // pitch envelope rates and levels
    31, 7, 1,                           // algorithm, feedback, key sync
    99, 99, 99, 99, 1, 5, 7,            // LFO
    48,                                 // transpose
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127,   // name
    63,                                 // operator on/off
};

static inline int fm_min(int a, int b) {
    return a < b ? a : b;
}

static inline int fm_max(int a, int b) {
    return a > b ? a : b;
}

// --- tables ----------------------------------------------------------------

// Q24 sine of a Q24 phase (2**24 is one cycle), interpolated.
static inline int32_t fm_sin(uint32_t phase) {
    const int shift = 24 - AUDIODSP_FM_TABLE_LG_N;
    int32_t lowbits = (int32_t)(phase & ((1u << shift) - 1u));
    uint32_t index = (phase >> (shift - 1)) &
        ((AUDIODSP_FM_TABLE_N - 1) << 1);
    int32_t dy = audiodsp_fm_sin_table[index];
    int32_t y0 = audiodsp_fm_sin_table[index + 1];
    return y0 + (int32_t)(((int64_t)dy * lowbits) >> shift);
}

// 2**(x / 2**24) in Q30 for the fractional part of x, interpolated.
static inline int32_t fm_exp2_frac(int32_t x) {
    const int shift = 24 - AUDIODSP_FM_TABLE_LG_N;
    int32_t lowbits = x & ((1 << shift) - 1);
    int32_t index = (x >> (shift - 1)) & ((AUDIODSP_FM_TABLE_N - 1) << 1);
    int32_t dy = audiodsp_fm_exp2_table[index];
    int32_t y0 = audiodsp_fm_exp2_table[index + 1];
    return y0 + (int32_t)(((int64_t)dy * lowbits) >> shift);
}

// 2**(x / 2**24) in Q24, for x up to six octaves; 0 far below that.
static inline int32_t fm_exp2(int32_t x) {
    int shift = 6 - (x >> 24);
    if (shift >= 31) {
        return 0;
    }
    if (shift < 0) {
        shift = 0;
    }
    return fm_exp2_frac(x) >> shift;
}

// Phase step per frame (Q24 of a cycle) for a Q24 log2 frequency in hertz.
static uint32_t fm_freq(const audiodsp_fm_synth_t *synth, int32_t logfreq) {
    if (logfreq < 0) {
        logfreq = 0;
    }
    int octave = logfreq >> 24;
    uint64_t t = ((uint64_t)(uint32_t)fm_exp2_frac(logfreq & 0xffffff) *
        synth->freq_scale) >> 30;
    if (octave >= 16) {
        t <<= (octave - 16);
    } else {
        t >>= (16 - octave);
    }
    return (uint32_t)t;
}

// --- envelope ----------------------------------------------------------------

static const uint8_t fm_level_lut[20] = {
    0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46
};

static int fm_scale_outlevel(int outlevel) {
    return outlevel >= 20 ? 28 + outlevel : fm_level_lut[outlevel];
}

static void fm_env_advance(audiodsp_fm_env_t *env, uint8_t stage) {
    env->stage = stage;
    if (stage < 4) {
        int actual = fm_scale_outlevel(env->levels[stage]) >> 1;
        actual = (actual << 6) + env->outlevel - 4256;
        actual = actual < 16 ? 16 : actual;
        env->target = actual << 16;
        env->rising = env->target > env->level;
        int qrate = (env->rates[stage] * 41) >> 6;
        qrate += env->rate_scaling;
        qrate = fm_min(qrate, 63);
        env->inc = (4 + (qrate & 3)) << (2 + FM_LG_N + (qrate >> 2));
    }
}

static void fm_env_init(audiodsp_fm_env_t *env, const uint8_t *rates,
    const uint8_t *levels, int32_t outlevel, int rate_scaling) {
    memcpy(env->rates, rates, 4);
    memcpy(env->levels, levels, 4);
    env->outlevel = outlevel;
    env->rate_scaling = (int8_t)rate_scaling;
    env->level = 0;
    env->down = true;
    fm_env_advance(env, 0);
}

static int32_t fm_env_sample(audiodsp_fm_env_t *env) {
    if (env->stage < 3 || (env->stage < 4 && !env->down)) {
        if (env->rising) {
            const int32_t jump = 1716 << 16;
            if (env->level < jump) {
                env->level = jump;
            }
            env->level += (((17 << 24) - env->level) >> 24) * env->inc;
            if (env->level >= env->target) {
                env->level = env->target;
                fm_env_advance(env, env->stage + 1);
            }
        } else {
            env->level -= env->inc;
            if (env->level <= env->target) {
                env->level = env->target;
                fm_env_advance(env, env->stage + 1);
            }
        }
    }
    return env->level;
}

static void fm_env_keydown(audiodsp_fm_env_t *env, bool down) {
    if (env->down != down) {
        env->down = down;
        fm_env_advance(env, down ? 0 : 3);
    }
}

// --- pitch envelope ------------------------------------------------------------

static const uint8_t fm_pitch_rate[100] = {
    1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
    12, 13, 13, 14, 14, 15, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 30, 31, 33, 34, 36, 37, 38, 39, 41, 42, 44, 46, 47,
    49, 51, 53, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 79, 82,
    85, 88, 91, 94, 98, 102, 106, 110, 115, 120, 125, 130, 135, 141, 147,
    153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255
};

static const int8_t fm_pitch_level[100] = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43,
    -41, -39, -37, -35, -33, -32, -31, -30, -29, -28, -27, -26, -25, -24,
    -23, -22, -21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11, -10,
    -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 38, 40, 43, 46, 49, 53, 58, 65, 73,
    82, 92, 103, 115, 127
};

static void fm_pitchenv_advance(audiodsp_fm_pitchenv_t *env, int32_t unit,
    uint8_t stage) {
    env->stage = stage;
    if (stage < 4) {
        env->target = (int32_t)fm_pitch_level[env->levels[stage]] * (1 << 19);
        env->rising = env->target > env->level;
        env->inc = fm_pitch_rate[env->rates[stage]] * unit;
    }
}

static void fm_pitchenv_init(audiodsp_fm_pitchenv_t *env, int32_t unit,
    const uint8_t *rates, const uint8_t *levels) {
    memcpy(env->rates, rates, 4);
    memcpy(env->levels, levels, 4);
    env->level = (int32_t)fm_pitch_level[levels[3]] * (1 << 19);
    env->down = true;
    fm_pitchenv_advance(env, unit, 0);
}

static int32_t fm_pitchenv_sample(audiodsp_fm_pitchenv_t *env, int32_t unit) {
    if (env->stage < 3 || (env->stage < 4 && !env->down)) {
        if (env->rising) {
            env->level += env->inc;
            if (env->level >= env->target) {
                env->level = env->target;
                fm_pitchenv_advance(env, unit, env->stage + 1);
            }
        } else {
            env->level -= env->inc;
            if (env->level <= env->target) {
                env->level = env->target;
                fm_pitchenv_advance(env, unit, env->stage + 1);
            }
        }
    }
    return env->level;
}

static void fm_pitchenv_keydown(audiodsp_fm_pitchenv_t *env, int32_t unit,
    bool down) {
    if (env->down != down) {
        env->down = down;
        fm_pitchenv_advance(env, unit, down ? 0 : 3);
    }
}

// --- LFO -----------------------------------------------------------------------

static void fm_lfo_reset(audiodsp_fm_lfo_t *lfo, uint32_t unit,
    const uint8_t *patch) {
    int rate = patch[AUDIODSP_FM_LFO_SPEED];
    int sr = rate == 0 ? 1 : (165 * rate) >> 6;
    sr *= sr < 160 ? 11 : (11 + ((sr - 160) >> 4));
    lfo->delta = unit * (uint32_t)sr;
    int a = 99 - patch[AUDIODSP_FM_LFO_DELAY];
    if (a == 99) {
        lfo->delay_inc = ~0u;
        lfo->delay_inc2 = ~0u;
    } else {
        a = (16 + (a & 15)) << (1 + (a >> 4));
        lfo->delay_inc = unit * (uint32_t)a;
        a &= 0xff80;
        a = fm_max(0x80, a);
        lfo->delay_inc2 = unit * (uint32_t)a;
    }
    lfo->wave = patch[AUDIODSP_FM_LFO_WAVE];
    lfo->sync = patch[AUDIODSP_FM_LFO_KEY_SYNC] != 0;
}

// 0..1 in Q24.
static int32_t fm_lfo_sample(audiodsp_fm_lfo_t *lfo) {
    lfo->phase += lfo->delta;
    uint32_t phase = lfo->phase;
    int32_t x;
    switch (lfo->wave) {
        case 0:     // triangle
            x = (int32_t)(phase >> 7);
            x ^= -(int32_t)(phase >> 31);
            x &= (1 << 24) - 1;
            return x;
        case 1:     // saw down
            return (int32_t)((~phase ^ (1u << 31)) >> 8);
        case 2:     // saw up
            return (int32_t)((phase ^ (1u << 31)) >> 8);
        case 3:     // square
            return (int32_t)(((~phase) >> 7) & (1u << 24));
        case 4:     // sine
            return (1 << 23) + (fm_sin(phase >> 8) >> 1);
        case 5:     // sample and hold
            if (phase < lfo->delta) {
                lfo->random = (uint8_t)((lfo->random * 179 + 17) & 0xff);
            }
            x = lfo->random ^ 0x80;
            return (x + 1) << 16;
        default:
            return 1 << 23;
    }
}

// The delay's fade-in, 0..1 in Q24.
static int32_t fm_lfo_delay(audiodsp_fm_lfo_t *lfo) {
    uint32_t delta = lfo->delay_state < (1u << 31) ? lfo->delay_inc :
        lfo->delay_inc2;
    uint32_t d = lfo->delay_state + delta;
    if (d < lfo->delay_inc) {
        return 1 << 24;
    }
    lfo->delay_state = d;
    if (d < (1u << 31)) {
        return 0;
    }
    return (int32_t)((d >> 7) & ((1u << 24) - 1));
}

static void fm_lfo_keydown(audiodsp_fm_lfo_t *lfo) {
    if (lfo->sync) {
        lfo->phase = (1u << 31) - 1;
    }
    lfo->delay_state = 0;
}

// --- operators -----------------------------------------------------------------

static void fm_op_compute(int32_t *output, const int32_t *input,
    uint32_t phase, uint32_t freq, int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (FM_N >> 1)) >> FM_LG_N;
    int32_t gain = gain1;
    for (int i = 0; i < FM_N; i++) {
        gain += dgain;
        int32_t y = fm_sin(phase + (uint32_t)input[i]);
        int32_t v = (int32_t)(((int64_t)y * gain) >> 24);
        output[i] = add ? output[i] + v : v;
        phase += freq;
    }
}

static void fm_op_compute_pure(int32_t *output, uint32_t phase, uint32_t freq,
    int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (FM_N >> 1)) >> FM_LG_N;
    int32_t gain = gain1;
    for (int i = 0; i < FM_N; i++) {
        gain += dgain;
        int32_t y = fm_sin(phase);
        int32_t v = (int32_t)(((int64_t)y * gain) >> 24);
        output[i] = add ? output[i] + v : v;
        phase += freq;
    }
}

static void fm_op_compute_fb(int32_t *output, uint32_t phase, uint32_t freq,
    int32_t gain1, int32_t gain2, int32_t *fb, int fb_shift, bool add) {
    int32_t dgain = (gain2 - gain1 + (FM_N >> 1)) >> FM_LG_N;
    int32_t gain = gain1;
    int32_t y0 = fb[0];
    int32_t y = fb[1];
    for (int i = 0; i < FM_N; i++) {
        gain += dgain;
        int32_t scaled = (y0 + y) >> (fb_shift + 1);
        y0 = y;
        y = fm_sin(phase + (uint32_t)scaled);
        y = (int32_t)(((int64_t)y * gain) >> 24);
        output[i] = add ? output[i] + y : y;
        phase += freq;
    }
    fb[0] = y0;
    fb[1] = y;
}

// --- algorithms ----------------------------------------------------------------

// One byte per operator, sixth operator first: bits 0-1 the bus it writes
// (0 is the output), bit 2 add rather than overwrite, bits 4-5 the bus it
// reads, bit 6 it reads the feedback loop, bit 7 it writes it.
enum {
    FM_OUT_BUS_ADD = 1 << 2,
    FM_FB_IN = 1 << 6,
    FM_FB_OUT = 1 << 7,
};

static const uint8_t fm_algorithms[32][6] = {
    { 0xc1, 0x11, 0x11, 0x14, 0x01, 0x14 },     // 1
    { 0x01, 0x11, 0x11, 0x14, 0xc1, 0x14 },     // 2
    { 0xc1, 0x11, 0x14, 0x01, 0x11, 0x14 },     // 3
    { 0x41, 0x11, 0x94, 0x01, 0x11, 0x14 },     // 4
    { 0xc1, 0x14, 0x01, 0x14, 0x01, 0x14 },     // 5
    { 0x41, 0x94, 0x01, 0x14, 0x01, 0x14 },     // 6
    { 0xc1, 0x11, 0x05, 0x14, 0x01, 0x14 },     // 7
    { 0x01, 0x11, 0xc5, 0x14, 0x01, 0x14 },     // 8
    { 0x01, 0x11, 0x05, 0x14, 0xc1, 0x14 },     // 9
    { 0x01, 0x05, 0x14, 0xc1, 0x11, 0x14 },     // 10
    { 0xc1, 0x05, 0x14, 0x01, 0x11, 0x14 },     // 11
    { 0x01, 0x05, 0x05, 0x14, 0xc1, 0x14 },     // 12
    { 0xc1, 0x05, 0x05, 0x14, 0x01, 0x14 },     // 13
    { 0xc1, 0x05, 0x11, 0x14, 0x01, 0x14 },     // 14
    { 0x01, 0x05, 0x11, 0x14, 0xc1, 0x14 },     // 15
    { 0xc1, 0x11, 0x02, 0x25, 0x05, 0x14 },     // 16
    { 0x01, 0x11, 0x02, 0x25, 0xc5, 0x14 },     // 17
    { 0x01, 0x11, 0x11, 0xc5, 0x05, 0x14 },     // 18
    { 0xc1, 0x14, 0x14, 0x01, 0x11, 0x14 },     // 19
    { 0x01, 0x05, 0x14, 0xc1, 0x14, 0x14 },     // 20
    { 0x01, 0x14, 0x14, 0xc1, 0x14, 0x14 },     // 21
    { 0xc1, 0x14, 0x14, 0x14, 0x01, 0x14 },     // 22
    { 0xc1, 0x14, 0x14, 0x01, 0x14, 0x04 },     // 23
    { 0xc1, 0x14, 0x14, 0x14, 0x04, 0x04 },     // 24
    { 0xc1, 0x14, 0x14, 0x04, 0x04, 0x04 },     // 25
    { 0xc1, 0x05, 0x14, 0x01, 0x14, 0x04 },     // 26
    { 0x01, 0x05, 0x14, 0xc1, 0x14, 0x04 },     // 27
    { 0x04, 0xc1, 0x11, 0x14, 0x01, 0x14 },     // 28
    { 0xc1, 0x14, 0x01, 0x14, 0x04, 0x04 },     // 29
    { 0x04, 0xc1, 0x11, 0x14, 0x04, 0x04 },     // 30
    { 0xc1, 0x14, 0x04, 0x04, 0x04, 0x04 },     // 31
    { 0xc4, 0x04, 0x04, 0x04, 0x04, 0x04 },     // 32
};

// Algorithms 4 and 6 close their feedback loop around two or three
// operators rather than one: the operator stored at 0 reads what the one at
// `end` wrote. The loop has to run a sample at a time, every operator in the
// chain in turn, so this computes the chain on its own. The chain always
// ends on a carrier.
static void fm_chain_compute_fb(audiodsp_fm_synth_t *synth,
    audiodsp_fm_voice_t *voice, int end) {
    int32_t gain[3], dgain[3];
    uint32_t phase[3], freq[3];
    for (int k = 0; k <= end; k++) {
        audiodsp_fm_op_t *param = &voice->op[k];
        dgain[k] = (param->gain[1] - param->gain[0] + (FM_N >> 1)) >> FM_LG_N;
        gain[k] = param->gain[0];
        phase[k] = (uint32_t)param->phase;
        freq[k] = (uint32_t)param->freq;
        param->phase = (int32_t)((uint32_t)param->phase +
            ((uint32_t)param->freq << FM_LG_N));
    }
    const int shift = voice->fb_shift + 1;
    int32_t y0 = voice->fb[0];
    int32_t y = voice->fb[1];
    for (int i = 0; i < FM_N; i++) {
        int32_t mod = (y0 + y) >> shift;
        for (int k = 0; k <= end; k++) {
            gain[k] += dgain[k];
            int32_t v = fm_sin(phase[k] + (uint32_t)mod);
            mod = (int32_t)(((int64_t)v * gain[k]) >> 24);
            phase[k] += freq[k];
        }
        y0 = y;
        y = mod;
        synth->mix[i] += mod;
    }
    voice->fb[0] = y0;
    voice->fb[1] = y;
}

static void fm_core_compute(audiodsp_fm_synth_t *synth,
    audiodsp_fm_voice_t *voice) {
    const uint8_t *alg = fm_algorithms[voice->algorithm];
    bool has_contents[3] = { true, false, false };
    int first = 0;
    if ((alg[0] & (FM_FB_IN | FM_FB_OUT)) == FM_FB_IN &&
        voice->fb_shift < 16) {
        int end = 1;
        while (!(alg[end] & FM_FB_OUT)) {
            end++;
        }
        fm_chain_compute_fb(synth, voice, end);
        first = end + 1;
    }
    for (int op = first; op < 6; op++) {
        int flags = alg[op];
        bool add = (flags & FM_OUT_BUS_ADD) != 0;
        audiodsp_fm_op_t *param = &voice->op[op];
        int inbus = (flags >> 4) & 3;
        int outbus = flags & 3;
        int32_t *out = outbus == 0 ? synth->mix : synth->bus[outbus - 1];
        int32_t gain1 = param->gain[0];
        int32_t gain2 = param->gain[1];
        if (gain1 >= FM_LEVEL_THRESHOLD || gain2 >= FM_LEVEL_THRESHOLD) {
            if (!has_contents[outbus]) {
                add = false;
            }
            if (inbus == 0 || !has_contents[inbus]) {
                if ((flags & (FM_FB_IN | FM_FB_OUT)) ==
                    (FM_FB_IN | FM_FB_OUT) && voice->fb_shift < 16) {
                    fm_op_compute_fb(out, (uint32_t)param->phase,
                        (uint32_t)param->freq, gain1, gain2, voice->fb,
                        voice->fb_shift, add);
                } else {
                    fm_op_compute_pure(out, (uint32_t)param->phase,
                        (uint32_t)param->freq, gain1, gain2, add);
                }
            } else {
                fm_op_compute(out, synth->bus[inbus - 1],
                    (uint32_t)param->phase, (uint32_t)param->freq, gain1,
                    gain2, add);
            }
            has_contents[outbus] = true;
        } else if (!add) {
            has_contents[outbus] = false;
        }
        param->phase = (int32_t)((uint32_t)param->phase +
            ((uint32_t)param->freq << FM_LG_N));
    }
}

// --- the voice -----------------------------------------------------------------

static int32_t fm_note_logfreq(int note) {
    const int32_t base = 50857777;     // (1 << 24) * (log2(440) - 69 / 12)
    const int32_t step = (1 << 24) / 12;
    return base + step * note;
}

static const int32_t fm_coarse_ratio[32] = {
    -16777216, 0, 16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
    50331648, 53182516, 55732705, 58039632, 60145690, 62083076, 63876816,
    65546747, 67108864, 68576247, 69959732, 71268397, 72509921, 73690858,
    74816848, 75892776, 76922906, 77910978, 78860292, 79773775, 80654032,
    81503396, 82323963, 83117622
};

static int32_t fm_osc_logfreq(int note, int fixed, int coarse, int fine,
    int detune) {
    int32_t logfreq;
    if (!fixed) {
        logfreq = fm_note_logfreq(note);
        logfreq += fm_coarse_ratio[coarse & 31];
        logfreq += audiodsp_fm_fine_table[fine];
        logfreq += 12606 * (detune - 7);
    } else {
        // ((1 << 24) * log2(10) * .01) << 3: fixed frequencies run in decades
        // from 1 Hz, and fine moves within the decade.
        logfreq = (4458616 * ((coarse & 3) * 100 + fine)) >> 3;
        logfreq += detune > 7 ? 13457 * (detune - 7) : 0;
    }
    return logfreq;
}

static const uint8_t fm_velocity_curve[64] = {
    0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
    205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
    233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
    252, 253, 254
};

// The level change velocity makes, in microsteps.
static int fm_scale_velocity(int velocity, int sensitivity) {
    int clamped = fm_max(0, fm_min(127, velocity));
    int value = fm_velocity_curve[clamped >> 1] - 239;
    return ((sensitivity * value + 7) >> 3) << 4;
}

static int fm_scale_rate(int note, int sensitivity) {
    int x = fm_min(31, fm_max(0, note / 3 - 7));
    return (sensitivity * x) >> 3;
}

static const uint8_t fm_exp_scale[33] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66,
    80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250
};

static int fm_scale_curve(int group, int depth, int curve) {
    int scale;
    if (curve == 0 || curve == 3) {
        scale = (group * depth * 329) >> 12;
    } else {
        int raw = fm_exp_scale[fm_min(group, (int)sizeof(fm_exp_scale) - 1)];
        scale = (raw * depth * 329) >> 15;
    }
    if (curve < 2) {
        scale = -scale;
    }
    return scale;
}

static int fm_scale_level(int note, int breakpoint, int left_depth,
    int right_depth, int left_curve, int right_curve) {
    int offset = note - breakpoint - 17;
    if (offset >= 0) {
        return fm_scale_curve(offset / 3, right_depth, right_curve);
    }
    return fm_scale_curve((-offset) / 3, left_depth, left_curve);
}

static const uint8_t fm_pitch_sens[8] = { 0, 10, 20, 33, 55, 92, 153, 255 };

// How far each amplitude-modulation sensitivity lets the LFO pull an
// operator down at full depth, in 64ths of an octave of level: 0, 3, 9 and
// 24 dB.
static const uint8_t fm_amp_sens[4] = { 0, 32, 96, 255 };

static void fm_voice_start(audiodsp_fm_synth_t *synth,
    audiodsp_fm_voice_t *voice, const uint8_t *p, int note, int velocity) {
    int key = fm_max(0, fm_min(127,
        note + p[AUDIODSP_FM_TRANSPOSE] - 24));
    bool sync = p[AUDIODSP_FM_KEY_SYNC] != 0 || !voice->live;
    for (int op = 0; op < 6; op++) {
        const uint8_t *o = p + op * AUDIODSP_FM_OP_BYTES;
        int outlevel = fm_scale_outlevel(o[AUDIODSP_FM_OP_LEVEL]);
        outlevel += fm_scale_level(key, o[AUDIODSP_FM_OP_BREAKPOINT],
            o[AUDIODSP_FM_OP_LEFT_DEPTH], o[AUDIODSP_FM_OP_RIGHT_DEPTH],
            o[AUDIODSP_FM_OP_LEFT_CURVE], o[AUDIODSP_FM_OP_RIGHT_CURVE]);
        outlevel = fm_min(127, outlevel);
        outlevel = outlevel << 5;
        outlevel += fm_scale_velocity(velocity, o[AUDIODSP_FM_OP_VELOCITY]);
        outlevel = fm_max(0, outlevel);
        fm_env_init(&voice->env[op], o + AUDIODSP_FM_OP_RATES,
            o + AUDIODSP_FM_OP_LEVELS, outlevel,
            fm_scale_rate(key, o[AUDIODSP_FM_OP_RATE_SCALING]));
        voice->basepitch[op] = fm_osc_logfreq(key, o[AUDIODSP_FM_OP_FIXED],
            o[AUDIODSP_FM_OP_COARSE], o[AUDIODSP_FM_OP_FINE],
            o[AUDIODSP_FM_OP_DETUNE]);
        voice->amp_mod[op] = o[AUDIODSP_FM_OP_AMP_MOD];
        if (sync) {
            voice->op[op].phase = 0;
        }
        voice->op[op].gain[1] = 0;
    }
    fm_pitchenv_init(&voice->pitchenv, synth->pitchenv_unit,
        p + AUDIODSP_FM_PITCH_RATES, p + AUDIODSP_FM_PITCH_LEVELS);
    voice->algorithm = p[AUDIODSP_FM_ALGORITHM];
    int feedback = p[AUDIODSP_FM_FEEDBACK];
    voice->fb_shift = (int16_t)(feedback != 0 ? 8 - feedback : 16);
    voice->fb[0] = 0;
    voice->fb[1] = 0;
    voice->pitch_depth = p[AUDIODSP_FM_LFO_PITCH_DEPTH];
    voice->amp_depth = p[AUDIODSP_FM_LFO_AMP_DEPTH];
    voice->pitch_sens = fm_pitch_sens[p[AUDIODSP_FM_LFO_PITCH_SENS] & 7];
    voice->enabled = p[AUDIODSP_FM_OP_ENABLE];
    voice->note = (uint8_t)note;
}

static void fm_voice_keyup(audiodsp_fm_synth_t *synth,
    audiodsp_fm_voice_t *voice) {
    for (int op = 0; op < 6; op++) {
        fm_env_keydown(&voice->env[op], false);
    }
    fm_pitchenv_keydown(&voice->pitchenv, synth->pitchenv_unit, false);
}

static void fm_voice_compute(audiodsp_fm_synth_t *synth,
    audiodsp_fm_voice_t *voice, int32_t lfo_value, int32_t lfo_delay) {
    int32_t pitchmod = fm_pitchenv_sample(&voice->pitchenv,
        synth->pitchenv_unit);
    int wheel = synth->mod_wheel * 2;
    uint32_t pitch_depth = (uint32_t)fm_min(255,
        ((voice->pitch_depth * 165) >> 6) + wheel);
    uint32_t pmd = pitch_depth * (uint32_t)lfo_delay;
    int32_t senslfo = voice->pitch_sens * (lfo_value - (1 << 23));
    pitchmod += (int32_t)(((int64_t)pmd * senslfo) >> 39);
    pitchmod += synth->bend;
    uint32_t amp_depth = (uint32_t)fm_min(255,
        ((voice->amp_depth * 165) >> 6) + wheel);
    int32_t amod = (int32_t)(((uint64_t)(amp_depth * (uint32_t)lfo_delay) *
        (uint32_t)lfo_value) >> 32);
    for (int op = 0; op < 6; op++) {
        audiodsp_fm_op_t *param = &voice->op[op];
        param->gain[0] = param->gain[1];
        int32_t level = fm_env_sample(&voice->env[op]);
        level -= (int32_t)(((int64_t)amod * fm_amp_sens[voice->amp_mod[op]])
            >> 6);
        param->gain[1] = (voice->enabled & (1u << op)) ?
            fm_exp2(level - (14 * (1 << 24))) : 0;
        param->freq = (int32_t)fm_freq(synth, voice->basepitch[op] +
            pitchmod);
    }
    fm_core_compute(synth, voice);

    // A released voice whose every carrier has fallen silent, and will stay
    // silent, is free again.
    if (!voice->keydown && !voice->sustained) {
        const uint8_t *alg = fm_algorithms[voice->algorithm];
        for (int op = 0; op < 6; op++) {
            if ((alg[op] & 3) != 0) {
                continue;
            }
            const audiodsp_fm_env_t *env = &voice->env[op];
            if (voice->op[op].gain[1] >= FM_LEVEL_THRESHOLD ||
                (env->stage < 4 && env->rising)) {
                return;
            }
        }
        voice->live = false;
    }
}

// --- the synth -----------------------------------------------------------------

void audiodsp_fm_init(audiodsp_fm_synth_t *synth, uint32_t sample_rate,
    uint32_t channel_count, audiodsp_fm_voice_t *voices,
    uint32_t voice_count) {
    memset(synth, 0, sizeof(*synth));
    if (sample_rate < 1000u) {
        sample_rate = 1000u;
    }
    synth->sample_rate = sample_rate;
    synth->channel_count = channel_count == 2u ? 2u : 1u;
    synth->voices = voices;
    synth->voice_count = voice_count;
    // 2**32 / 15.5 s / 11 per frame, so an LFO at speed 0 runs about once a
    // minute.
    synth->lfo_unit = (uint32_t)((double)FM_N * 25190424.0 / sample_rate + 0.5);
    synth->pitchenv_unit = (int32_t)((double)FM_N * (1 << 24) /
        (21.3 * sample_rate) + 0.5);
    synth->freq_scale = (uint32_t)(((uint64_t)1 << 40) / sample_rate);
    memset(voices, 0, sizeof(*voices) * voice_count);
    uint8_t init[AUDIODSP_FM_PATCH_BYTES];
    audiodsp_fm_init_voice(init);
    fm_lfo_reset(&synth->lfo, synth->lfo_unit, init);
}

static uint32_t fm_pick_voice(audiodsp_fm_synth_t *synth) {
    const uint32_t count = synth->voice_count;
    uint32_t index = synth->next_voice % count;
    for (uint32_t i = 0; i < count; i++) {
        if (!synth->voices[index].live) {
            synth->next_voice = (index + 1) % count;
            return index;
        }
        index = (index + 1) % count;
    }
    // Every voice is sounding: take the oldest released one, then the
    // oldest held one.
    uint32_t best = 0;
    bool best_released = false;
    uint32_t best_age = 0;
    for (uint32_t i = 0; i < count; i++) {
        const audiodsp_fm_voice_t *v = &synth->voices[i];
        bool released = !v->keydown && !v->sustained;
        uint32_t age = synth->clock - v->age;
        if ((released && !best_released) ||
            (released == best_released && age > best_age)) {
            best = i;
            best_released = released;
            best_age = age;
        }
    }
    return best;
}

void audiodsp_fm_press(audiodsp_fm_synth_t *synth, const uint8_t *patch,
    uint8_t note, uint8_t velocity) {
    if (synth->voice_count == 0) {
        return;
    }
    uint8_t p[AUDIODSP_FM_PATCH_BYTES];
    memcpy(p, patch, AUDIODSP_FM_PATCH_BYTES);
    audiodsp_fm_clamp(p);
    if (note > 127) {
        note = 127;
    }
    if (velocity > 127) {
        velocity = 127;
    }
    // The LFO is the synth's, not the voice's: the newest note's patch sets
    // its speed, delay and wave.
    fm_lfo_reset(&synth->lfo, synth->lfo_unit, p);
    fm_lfo_keydown(&synth->lfo);
    audiodsp_fm_voice_t *voice = &synth->voices[fm_pick_voice(synth)];
    fm_voice_start(synth, voice, p, note, velocity);
    voice->keydown = true;
    voice->sustained = false;
    voice->live = true;
    voice->age = synth->clock++;
}

void audiodsp_fm_release(audiodsp_fm_synth_t *synth, uint8_t note) {
    for (uint32_t i = 0; i < synth->voice_count; i++) {
        audiodsp_fm_voice_t *voice = &synth->voices[i];
        if (voice->live && voice->keydown && voice->note == note) {
            voice->keydown = false;
            if (synth->sustain) {
                voice->sustained = true;
            } else {
                fm_voice_keyup(synth, voice);
            }
        }
    }
}

void audiodsp_fm_release_all(audiodsp_fm_synth_t *synth) {
    for (uint32_t i = 0; i < synth->voice_count; i++) {
        audiodsp_fm_voice_t *voice = &synth->voices[i];
        if (voice->live && (voice->keydown || voice->sustained)) {
            voice->keydown = false;
            voice->sustained = false;
            fm_voice_keyup(synth, voice);
        }
    }
}

void audiodsp_fm_reset(audiodsp_fm_synth_t *synth) {
    for (uint32_t i = 0; i < synth->voice_count; i++) {
        audiodsp_fm_voice_t *voice = &synth->voices[i];
        voice->live = false;
        voice->keydown = false;
        voice->sustained = false;
        voice->fb[0] = 0;
        voice->fb[1] = 0;
    }
}

void audiodsp_fm_set_sustain(audiodsp_fm_synth_t *synth, bool down) {
    synth->sustain = down;
    if (down) {
        return;
    }
    for (uint32_t i = 0; i < synth->voice_count; i++) {
        audiodsp_fm_voice_t *voice = &synth->voices[i];
        if (voice->live && voice->sustained && !voice->keydown) {
            voice->sustained = false;
            fm_voice_keyup(synth, voice);
        }
    }
}

void audiodsp_fm_set_bend(audiodsp_fm_synth_t *synth, int32_t bend_q24) {
    synth->bend = bend_q24;
}

void audiodsp_fm_set_mod_wheel(audiodsp_fm_synth_t *synth, uint8_t value) {
    synth->mod_wheel = value > 127 ? 127 : value;
}

uint32_t audiodsp_fm_active(const audiodsp_fm_synth_t *synth) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < synth->voice_count; i++) {
        count += synth->voices[i].live ? 1u : 0u;
    }
    return count;
}

void audiodsp_fm_render_s16(audiodsp_fm_synth_t *synth, int16_t *out,
    uint32_t frames) {
    const uint32_t channels = synth->channel_count;
    for (uint32_t done = 0; done + FM_N <= frames; done += FM_N) {
        memset(synth->mix, 0, sizeof(synth->mix));
        int32_t lfo_value = fm_lfo_sample(&synth->lfo);
        int32_t lfo_delay = fm_lfo_delay(&synth->lfo);
        for (uint32_t i = 0; i < synth->voice_count; i++) {
            if (synth->voices[i].live) {
                fm_voice_compute(synth, &synth->voices[i], lfo_value,
                    lfo_delay);
            }
        }
        int16_t *dst = out + done * channels;
        for (int i = 0; i < FM_N; i++) {
            int32_t v = synth->mix[i] >> 13;
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            for (uint32_t c = 0; c < channels; c++) {
                *dst++ = (int16_t)v;
            }
        }
    }
}

// --- the voice format ----------------------------------------------------------

void audiodsp_fm_clamp(uint8_t *patch) {
    for (size_t i = 0; i < AUDIODSP_FM_PATCH_BYTES; i++) {
        if (patch[i] > audiodsp_fm_patch_max[i]) {
            patch[i] = audiodsp_fm_patch_max[i];
        }
    }
}

void audiodsp_fm_pack_voice(const uint8_t *patch, uint8_t *packed) {
    for (int op = 0; op < 6; op++) {
        const uint8_t *o = patch + op * 21;
        uint8_t *d = packed + op * 17;
        memcpy(d, o, 11);
        d[11] = (uint8_t)(((o[12] & 3) << 2) | (o[11] & 3));
        d[12] = (uint8_t)(((o[20] & 15) << 3) | (o[13] & 7));
        d[13] = (uint8_t)(((o[15] & 7) << 2) | (o[14] & 3));
        d[14] = o[16];
        d[15] = (uint8_t)(((o[18] & 31) << 1) | (o[17] & 1));
        d[16] = o[19];
    }
    memcpy(packed + 102, patch + 126, 9);
    packed[111] = (uint8_t)(((patch[136] & 1) << 3) | (patch[135] & 7));
    memcpy(packed + 112, patch + 137, 4);
    packed[116] = (uint8_t)(((patch[143] & 7) << 4) | ((patch[142] & 7) << 1) |
        (patch[141] & 1));
    memcpy(packed + 117, patch + 144, 11);
    for (size_t i = 0; i < AUDIODSP_FM_PACKED_BYTES; i++) {
        packed[i] &= 0x7f;
    }
}

void audiodsp_fm_unpack_voice(const uint8_t *packed, uint8_t *patch) {
    for (int op = 0; op < 6; op++) {
        const uint8_t *s = packed + op * 17;
        uint8_t *o = patch + op * 21;
        for (int i = 0; i < 11; i++) {
            o[i] = s[i] & 0x7f;
        }
        o[11] = s[11] & 3;
        o[12] = (s[11] >> 2) & 3;
        o[13] = s[12] & 7;
        o[20] = (s[12] >> 3) & 15;
        o[14] = s[13] & 3;
        o[15] = (s[13] >> 2) & 7;
        o[16] = s[14] & 0x7f;
        o[17] = s[15] & 1;
        o[18] = (s[15] >> 1) & 31;
        o[19] = s[16] & 0x7f;
    }
    for (int i = 0; i < 9; i++) {
        patch[126 + i] = packed[102 + i] & 0x7f;
    }
    patch[135] = packed[111] & 7;
    patch[136] = (packed[111] >> 3) & 1;
    for (int i = 0; i < 4; i++) {
        patch[137 + i] = packed[112 + i] & 0x7f;
    }
    patch[141] = packed[116] & 1;
    patch[142] = (packed[116] >> 1) & 7;
    patch[143] = (packed[116] >> 4) & 7;
    for (int i = 0; i < 11; i++) {
        patch[144 + i] = packed[117 + i] & 0x7f;
    }
    patch[AUDIODSP_FM_OP_ENABLE] = 0x3f;
    audiodsp_fm_clamp(patch);
}

uint8_t audiodsp_fm_checksum(const uint8_t *data, size_t length) {
    uint32_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += data[i];
    }
    return (uint8_t)((128u - (sum & 127u)) & 127u);
}

void audiodsp_fm_init_voice(uint8_t *patch) {
    static const uint8_t op[21] = {
        99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7
    };
    for (int i = 0; i < 6; i++) {
        memcpy(patch + i * 21, op, 21);
    }
    // Operator 1 is stored last, and is the only one sounding.
    patch[5 * 21 + AUDIODSP_FM_OP_LEVEL] = 99;
    static const uint8_t global[19] = {
        99, 99, 99, 99, 50, 50, 50, 50,     // pitch envelope
        0, 0, 1,                            // algorithm 1, feedback, key sync
        35, 0, 0, 0, 1, 0, 3,               // LFO
        24,                                 // transpose
    };
    memcpy(patch + 126, global, sizeof(global));
    memcpy(patch + AUDIODSP_FM_NAME, "INIT VOICE", AUDIODSP_FM_NAME_BYTES);
    patch[AUDIODSP_FM_OP_ENABLE] = 0x3f;
}
