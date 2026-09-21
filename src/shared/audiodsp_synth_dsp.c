// SPDX-License-Identifier: MIT

#include "shared/audiodsp_synth_dsp.h"

static const uint16_t pitch_bend_table[] = {
    0, 1948, 4013, 6200, 8517, 10972, 13573,
    16329, 19248, 22341, 25618, 29090, 32768
};

int16_t audiodsp_sat16(int32_t value, int right_shift) {
    if (value < 0) {
        value += ~(0xFFFFFFFFUL << right_shift);
    }
    value >>= right_shift;
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

int16_t audiodsp_mix_down_sample(int32_t sample, int32_t scale,
    int32_t range_low, int32_t range_high) {
    if (sample < range_low) {
        sample = (((sample - range_low) * scale) >> 16) + range_low;
    } else if (sample > range_high) {
        sample = (((sample - range_high) * scale) >> 16) + range_high;
    }
    return (int16_t)sample;
}

bool audiodsp_oscillator_fill(int32_t *output, const int16_t *waveform,
    uint32_t waveform_start, uint32_t waveform_end, uint32_t dds_rate,
    uint32_t *accumulator, uint16_t duration, uint8_t frequency_shift) {
    uint32_t offset = waveform_start << frequency_shift;
    uint32_t limit = waveform_end << frequency_shift;
    uint32_t span = limit - offset;
    uint32_t accum = *accumulator;
    if (dds_rate > limit / 2) return false;
    // Deviation from CircuitPython: it wraps on `accum > limit`, which lets an
    // accumulator landing exactly on the loop end index waveform[waveform_end]
    // - one past the samples it may read, and off the end of the buffer
    // entirely for a note looping the whole waveform. Any table advancing an
    // exact number of samples per frame hits it, so a render's output depended
    // on whatever the allocator had left after the array. The playable index
    // range is [waveform_start, waveform_end), so wrap on `>=`.
    // See docs/upstream-diff.md.
    if (accum >= limit) accum = offset + (accum - offset) % span;

    for (uint16_t i = 0; i < duration; i++) {
        accum += dds_rate;
        if (accum >= limit) accum -= span;
        output[i] = waveform[accum >> frequency_shift];
    }
    *accumulator = accum;
    return true;
}

// Loudness changes wait for a zero crossing -- the sample is zero, or its
// sign differs from the previous one -- so an amplitude or pan move cannot
// step the waveform mid-cycle and click. CircuitPython 10.3.0 added this
// (shared-module/synthio/__init__.c, `assign_loudness`); the sign test is
// copied as written there so the two stay byte-identical.
void audiodsp_assign_loudness(int32_t word, int32_t *last_word,
    int16_t active_loudness[2], const int16_t pending_loudness[2]) {
    if (active_loudness[0] == pending_loudness[0] &&
        active_loudness[1] == pending_loudness[1]) {
        return;
    }
    if (word == 0 || (*last_word != 0 &&
                      ((*last_word > 0) == (word < 0) ||
                       (*last_word < 0) == (word > 0)))) {
        active_loudness[0] = pending_loudness[0];
        active_loudness[1] = pending_loudness[1];
    } else {
        *last_word = word;
    }
}

// The same zero-crossing gate for audiomixer, whose samples arrive two per
// 32-bit word. The zero and sign tests are done on the packed pair, so a word
// with either channel at zero counts as a crossing -- copied as written in
// CircuitPython 10.3.0, shared-module/audiomixer/Mixer.c `assignmul`.
void audiodsp_assign_packed_level(uint32_t word, uint32_t *last_word,
    int32_t *active_lo, int32_t *active_hi,
    int32_t pending_lo, int32_t pending_hi) {
    if (*active_lo == pending_lo && *active_hi == pending_hi) {
        return;
    }
    if ((word & 0xffff0000) == 0 || (word & 0x0000ffff) == 0 ||
        (*last_word != 0 &&
         (*last_word & 0x80008000) != (word & 0x80008000))) {
        *active_lo = pending_lo;
        *active_hi = pending_hi;
    } else {
        *last_word = word;
    }
}

void audiodsp_sum_with_loudness(int32_t *output, const int32_t *voice,
    int16_t active_loudness[2], const int16_t pending_loudness[2],
    size_t duration, uint8_t channel_count) {
    int32_t word, last_word = 0;
    if (channel_count == 1) {
        for (size_t i = 0; i < duration; i++) {
            word = *voice++;
            audiodsp_assign_loudness(word, &last_word, active_loudness,
                pending_loudness);
            *output++ += audiodsp_sat16(word * active_loudness[0], 16);
        }
    } else {
        for (size_t i = 0; i < duration; i++) {
            word = *voice;
            audiodsp_assign_loudness(word, &last_word, active_loudness,
                pending_loudness);
            *output++ += audiodsp_sat16(word * active_loudness[0], 16);
            *output++ += audiodsp_sat16(word * active_loudness[1], 16);
            voice++;
        }
    }
    // A cycle need not contain a crossing inside one block. Rather than let a
    // level change hang indefinitely, force it at the block boundary: at most
    // one block of delay, at the cost of a possible click.
    active_loudness[0] = pending_loudness[0];
    active_loudness[1] = pending_loudness[1];
}

uint32_t audiodsp_pitch_bend(uint32_t frequency_scaled, int32_t bend_value) {
    int octave = bend_value >> 15;
    bend_value &= 0x7fff;
    uint32_t bend_value_semitone = (uint32_t)bend_value * 24;
    uint32_t semitone = bend_value_semitone >> 16;
    uint32_t fractone = bend_value_semitone & 0xffff;
    uint32_t low = pitch_bend_table[semitone];
    uint32_t high = pitch_bend_table[semitone + 1];
    uint32_t factor = ((low * (65535 - fractone) + high * fractone) >> 16) + 32768;
    return (uint32_t)((frequency_scaled * (uint64_t)factor) >> (15 - octave));
}
