// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_pitchshift.h"
#include "shared/audiodsp_synth_dsp.h"

#define READ_SHIFT 8

// The splice. Upstream crossfades from what the read was playing to what is
// a whole window older, over `overlap` frames, wherever that lands: on every
// note whose period goes into the window an odd number of halves, the two
// sides are in antiphase and the crossfade cancels the note for its length,
// once a splice. Here the incoming side is moved, by up to `align_span`
// frames and only ever toward audio that is there, to where the next
// ALIGN_LENGTH frames it plays differ least from the outgoing side's (summed
// absolute differences, first channel, in integers, so every interpreter
// moves it to the same frame). The move holds while the crossfade runs and
// is then taken into the read position, so the read carries on from what it
// faded in. A tie, silence included, keeps the smaller move: a quiet window
// splices exactly where upstream's does, with the same bytes.
#define ALIGN_LENGTH 32
#define ALIGN_STRIDE 6

// The input `age` frames ago, 0 being the frame being written now. The
// overlap buffer holds ages 0 .. overlap - 1 and the window the next
// `window` ages, as the writes above leave them.
static int32_t history(const int16_t *window, uint32_t window_samples,
    const int16_t *overlap, uint32_t overlap_samples,
    const audiodsp_pitchshift_positions_t *positions, uint8_t channel,
    uint32_t age) {
    if (age < overlap_samples) {
        return overlap[(positions->overlap_index + overlap_samples - age) %
            overlap_samples + overlap_samples * channel];
    }
    age -= overlap_samples;
    return window[(positions->window_index + window_samples - age %
        window_samples) % window_samples + window_samples * channel];
}

// Rising (the read gains on the write): the outgoing side is the overlap
// buffer at `off`, the incoming the window at read + move, both played
// forward. Falling: the outgoing side is the window at read and the
// incoming the overlap buffer at `off`, older by `move`; compared over the
// frames just played, since the incoming side starts on the newest audio
// there is.
static uint32_t splice_move(const int16_t *window, uint32_t window_samples,
    const int16_t *overlap, uint32_t overlap_samples, uint8_t channel_count,
    const audiodsp_pitchshift_positions_t *positions, uint32_t read,
    uint32_t off, bool rising, uint32_t span) {
    uint32_t length;
    if (rising) {
        // The outgoing frames ahead must have been written: ages down to 1.
        length = overlap_samples > off + 1u ? overlap_samples - off - 1u : 0u;
    } else {
        // The outgoing frames behind must still be in the window.
        length = off > 1u ? off - 1u : 0u;
    }
    if (length > ALIGN_LENGTH) {
        length = ALIGN_LENGTH;
    }
    // The incoming side may not run past either end of what is held, for as
    // long as the crossfade lasts.
    const uint32_t room = window_samples > overlap_samples + length + 2u ?
        window_samples - overlap_samples - length - 2u : 0u;
    if (span > room) {
        span = room;
    }
    if (length < 8u || span == 0u) {
        return 0u;
    }
    uint32_t best = 0;
    uint64_t best_score = 0;
    bool found = false;
    uint32_t first = 0, last = span, stride = ALIGN_STRIDE;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t move = first; move <= last; move += stride) {
            if (pass == 1 && move == best) {
                continue;
            }
            uint64_t score = 0;
            // The first channel alone decides, so a stereo node splices its
            // left channel exactly as a mono node fed that channel does.
            for (uint8_t channel = 0; channel < 1u; ++channel) {
                const int16_t *lane = window + window_samples * channel;
                for (uint32_t k = 0; k < length; ++k) {
                    int32_t out, in;
                    if (rising) {
                        out = overlap[(positions->overlap_index + off + k) %
                            overlap_samples + overlap_samples * channel];
                        in = lane[(read + move + k) % window_samples];
                    } else {
                        out = lane[(read + window_samples - 1u - k) %
                            window_samples];
                        in = history(window, window_samples, overlap,
                            overlap_samples, positions, channel,
                            overlap_samples - off + move + 1u + k);
                    }
                    const int32_t gap = out - in;
                    score += (uint32_t)(gap < 0 ? -gap : gap);
                }
            }
            if (!found || score < best_score) {
                found = true;
                best = move;
                best_score = score;
            }
        }
        // Then frame by frame around the best of the coarse pass.
        first = best > ALIGN_STRIDE - 1u ? best - (ALIGN_STRIDE - 1u) : 0u;
        last = best + (ALIGN_STRIDE - 1u) < span ?
            best + (ALIGN_STRIDE - 1u) : span;
        stride = 1;
    }
    return best;
}

void audiodsp_pitchshift_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *window, uint32_t window_samples,
    int16_t *overlap, uint32_t overlap_samples, uint8_t channel_count,
    uint32_t read_rate, double mix, bool freeze, uint32_t align_span,
    audiodsp_pitchshift_positions_t *positions) {
    int32_t scale = 0xfffffff / (32768 * 2 - 28000);
    // Which way the read moves against the write. A frozen window is a loop
    // whose length is the window, and it splices where upstream's does: the
    // move is for audio that is still coming in, and a crossfade begun
    // before a freeze is finished and taken into the read once, not left to
    // apply on every lap.
    const bool rising = read_rate > (1u << READ_SHIFT);
    const bool aligning = overlap_samples > 0u && align_span > 0u &&
        !freeze && read_rate != (1u << READ_SHIFT);
    for (size_t i = 0; i < sample_count; i++) {
        uint8_t channel = (uint8_t)(i % channel_count);
        uint32_t window_plane = window_samples * channel;
        uint32_t overlap_plane = overlap_samples * channel;
        int32_t sample = input[i];
        // A frozen window takes no new audio and its write pointers stand
        // still, so the read pointer keeps cycling over what it holds
        // (upstream 9bef7b7606).
        if (!freeze) {
            if (overlap_samples) {
                window[positions->window_index + window_plane] =
                    overlap[positions->overlap_index + overlap_plane];
                overlap[positions->overlap_index + overlap_plane] =
                    (int16_t)sample;
            } else {
                window[positions->window_index + window_plane] =
                    (int16_t)sample;
            }
        }
        uint32_t read = positions->read_index >> READ_SHIFT;
        uint32_t overlap_offset = read +
            window_samples * (read < positions->window_index) -
            positions->window_index;
        // A crossfade under way is finished whatever happens to the rate or
        // to `freeze`; only a new one waits for `aligning`.
        if (channel == 0 && (aligning || positions->fading)) {
            const bool inside = overlap_offset > 0 &&
                overlap_offset <= overlap_samples;
            if (inside && !positions->fading && aligning) {
                positions->fading = true;
                positions->fading_rising = rising;
                positions->align = splice_move(window, window_samples,
                    overlap, overlap_samples, channel_count, positions, read,
                    overlap_offset, rising, align_span);
            } else if (!inside && positions->fading) {
                // The crossfade is over. Left past the overlap, a rising
                // fade has faded the moved side in; left at the write
                // position, a falling one has. Then the read carries on from
                // what it faded in, so the move goes into the read position.
                // Left the other way, the rate turned round mid-fade and the
                // moved side faded back out to nothing: the move is dropped,
                // and nothing it touched is playing.
                const bool past = overlap_offset > overlap_samples &&
                    overlap_offset <= overlap_samples +
                    (window_samples - overlap_samples) / 2u;
                const bool completed = positions->fading_rising == past;
                const uint32_t whole = window_samples << READ_SHIFT;
                const uint32_t moved = completed ?
                    positions->align << READ_SHIFT : 0u;
                if (positions->fading_rising) {
                    positions->read_index += moved;
                    if (positions->read_index >= whole) {
                        positions->read_index -= whole;
                    }
                } else {
                    positions->read_index += whole - moved;
                    if (positions->read_index >= whole) {
                        positions->read_index -= whole;
                    }
                }
                positions->fading = false;
                positions->align = 0;
                read = positions->read_index >> READ_SHIFT;
                overlap_offset = read +
                    window_samples * (read < positions->window_index) -
                    positions->window_index;
            }
        }
        const uint32_t move = positions->fading ? positions->align : 0u;
        int32_t word;
        if (overlap_samples && overlap_offset > 0 &&
            overlap_offset <= overlap_samples) {
            int32_t windowed, overlapped;
            if (move == 0u || positions->fading_rising) {
                windowed = window[(read + move) % window_samples +
                    window_plane];
                overlapped = overlap[(positions->overlap_index +
                    overlap_offset) % overlap_samples + overlap_plane];
            } else {
                windowed = window[read + window_plane];
                overlapped = move ? history(window, window_samples, overlap,
                    overlap_samples, positions, channel,
                    overlap_samples - overlap_offset + move) :
                    overlap[(positions->overlap_index + overlap_offset) %
                        overlap_samples + overlap_plane];
            }
            word = windowed * (int32_t)overlap_offset;
            word += overlapped * (int32_t)(overlap_samples - overlap_offset);
            word /= (int32_t)overlap_samples;
        } else if (freeze && overlap_samples && overlap_offset == 0) {
            // Frozen, nothing is written at the write position, so the
            // window there still holds its oldest frame; the frame that
            // follows the newest is the overlap's oldest. Upstream plays the
            // window's, a click on every lap whose read lands exactly on the
            // write position (docs/upstream-diff.md).
            word = overlap[positions->overlap_index + overlap_plane];
        } else {
            word = window[read + window_plane];
        }
        double dry = 2.0 - mix < 1.0 ? 2.0 - mix : 1.0;
        double wet = mix < 1.0 ? mix : 1.0;
        word = (int32_t)(sample * dry + word * wet);
        output[i] = audiodsp_mix_down_sample(word, scale, -28000, 28000);
        if (channel_count == 1 || channel == 1) {
            if (!freeze) {
                if (++positions->window_index >= window_samples) {
                    positions->window_index = 0;
                }
                if (overlap_samples &&
                    ++positions->overlap_index >= overlap_samples) {
                    positions->overlap_index = 0;
                }
            }
            positions->read_index += read_rate;
            if (positions->read_index >= window_samples << READ_SHIFT) {
                positions->read_index -= window_samples << READ_SHIFT;
            }
        }
    }
}
