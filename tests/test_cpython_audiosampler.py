"""audiosampler: the traits and the surface the renders cannot check.

`tests/parity/sampler_probe.py` pins what this module renders across the
three targets. What is left for here is what a render cannot say, and the
numeric traits with their bars.

`docs/how-it-works/correctness-standard.md` is what this file implements for
`audiosampler`: the module is ours - upstream CircuitPython has no
counterpart - so it is held to three-target agreement plus the traits below,
never to a previous version of its own output.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| S1 | A looped tone plays at the pitch asked for, over two octaves | 0.05% |
| S2 | `rate` rescales the step: a 24 kHz buffer through 48 kHz plays at half speed | 0.05% |
| S3 | At pitch 1.0 a loop is a wire: the buffer's own values come out | exact |
| S4 | `LOOP_NONE` stops at the end; `LOOP_FOREVER` does not stop | exact |
| S5 | `LOOP_UNTIL_RELEASE` never leaves the loop while held, and plays on to the end after `release()` | exact |
| S6 | `release()` fades linearly over `release` seconds and `playing` then goes False | 2% of the time, exact on `playing` |
| S7 | `stop()` frees the voice at once and fades the note to zero within 5 ms: monotone, no step larger than the ramp; `deinit()` still cuts | exact |
| S8 | Attack, decay and sustain land where asked | 2% |
| S9 | `delay` holds the sound back, not just the envelope | within one frame |
| S10 | Pan is a linear balance: -1 is silence on the right; 0 is the level on both | exact |
| S11 | A live level change is a ramp across one block, not a step | monotone, exact at the ends |
| S12 | A live pitch change takes effect from the next block | S1's bar |
| S13 | Voices sum linearly, and the sum clamps at the rails rather than wrapping | 1 LSB; exact |
| S14 | A loop that does not fit, or a mode that is not 0, 1 or 3, is refused | raises |
| S15 | The buffer is held while the voice sounds or its stop fade plays, and let go when the fade ends | exact |
| S16 | A Sampler plays into a Mixer like any other source | non-silent |
| S17 | 24 looping stereo voices render inside the 5333 us block | measured |
| S18 | `play()` over a sounding voice fades the old note out under the new one, as S7 | exact |
| S19 | A voice already releasing keeps releasing when stopped, no slower than the 5 ms fade | exact |
| S20 | A Sampler of 128 voices builds, sounds on every voice and sums them; one more than `MAX_VOICES` is refused | exact; raises |
| S21 | The first 64 voices of a table of 128 render what a table of 64 does | exact |
| S22 | A steal on the last voice of a table of 128 fades the old note out, as S18 | exact |
| S23 | 128 looping stereo voices render inside the 5333 us block | measured |

S3 is this module's form of the identity trait `docs/how-it-works/correctness-standard.md`
asks of every own node: an exact answer through the interpolator rather than
around it. The Hermite polynomial passes through its samples, so an integer
position reads the buffer's own value, and a loop at pitch 1.0 whose length
is a whole number of frames stays on integer positions for ever.

S1 is the gate the sampler roadmap names: pitch by measurement, not by
inspection of the step. The tone is a single 128-frame cycle of a sine at
48 kHz, 375 Hz, looped and played at every semitone from an octave down to
an octave up, and the frequency is counted off the output's zero crossings.

## The planted fault

`docs/how-it-works/correctness-standard.md`: "a trait without a planted fault is not a
check; it is a hope." The fault below is a one-line change to
`src/shared/audiodsp_sampler.c` that a reader can make by hand, with what it
did when it was made. It is recorded rather than automated because the file
it breaks is C: the harness that would plant it is a rebuild.

| Fault | Breaks | What was measured |
|---|---|---|
| `hand_to_tail()` sends the voice straight to idle, the cut every stop was before the fade | S7, S15, S18, S19 | `Sampler.playing` is already False the moment `stop()` returns, with a fade still owed; S18's old note drops from 20000 to 0 in one frame under the new one, against a bar of 85; S19's slow release is cut the same way (19894 in one frame, bar 84) and its fast one never sounds; S15's buffer is free the moment `stop()` returns. Measured 2026-10-19, when the fade went in. |
| `step_for()` scales the step by 1.01 | S1, S2, S3, S4, S9, S10, S12, S14, S16 | every pitch lands 1% high: 187.5 Hz asked, 189.37 measured; 375 asked, 378.77 measured; the wire S3 fails because the position leaves the integers (981 asked, 991 read), and every trait that compares a block against the buffer's own values fails with it. Nine of seventeen. |
"""

import math
import time
import unittest
from array import array

import audiocore
import audiomixer
import audiosampler

RATE = 48000
BLOCK = audiosampler.FRAMES
FADE = 240            # 5 ms at 48 kHz: the stop fade, in frames
CYCLE = 128           # frames in one period of the test tone
TONE_HZ = RATE / CYCLE  # 375 Hz


def sine_cycle(frames=CYCLE, peak=20000):
    return array("h", [int(round(peak * math.sin(2 * math.pi * i / frames)))
                       for i in range(frames)])


def render(sampler, blocks):
    """Interleaved samples, as ints, from `blocks` pulls."""
    out = []
    for _ in range(blocks):
        result, data = audiocore.get_buffer(sampler)
        out.extend(array("h", bytes(data)))
    return out


def channel(samples, which, channels=2):
    return samples[which::channels]


def zero_crossing_hz(samples, skip=BLOCK, rate=RATE):
    """Frequency from counting rising zero crossings -- no FFT, no window."""
    body = samples[skip:]
    crossings = [i for i in range(1, len(body))
                 if body[i - 1] < 0 <= body[i]]
    if len(crossings) < 2:
        return 0.0
    return (len(crossings) - 1) * rate / float(crossings[-1] - crossings[0])


def flat(value=20000, frames=CYCLE):
    """A loop that reads one value, so the output is the envelope times it."""
    return array("h", [value] * frames)


def peak(samples):
    return max(abs(v) for v in samples) if samples else 0


def mono(voices=1, **kw):
    return audiosampler.Sampler(voice_count=voices, channel_count=1,
                                sample_rate=RATE, **kw)


class SamplerTraits(unittest.TestCase):

    def assertHz(self, samples, want, rel=0.0005):
        got = zero_crossing_hz(samples)
        self.assertAlmostEqual(got, want, delta=want * rel,
                               msg="measured %.3f Hz, wanted %.3f" % (got, want))

    def test_s1_pitch_across_two_octaves(self):
        sampler = mono()
        for semitones in range(-12, 13):
            pitch = 2.0 ** (semitones / 12.0)
            sampler.voice[0].play(sine_cycle(), pitch=pitch,
                                  loop_mode=audiosampler.LOOP_FOREVER)
            out = render(sampler, 40)
            self.assertHz(out, TONE_HZ * pitch)
            sampler.voice[0].stop()

    def test_s2_rate_rescales_the_step(self):
        sampler = mono()
        sampler.voice[0].play(sine_cycle(), rate=24000,
                              loop_mode=audiosampler.LOOP_FOREVER)
        self.assertHz(render(sampler, 40), TONE_HZ / 2)
        sampler.voice[0].play(sine_cycle(), rate=96000, pitch=0.5,
                              loop_mode=audiosampler.LOOP_FOREVER)
        self.assertHz(render(sampler, 40), TONE_HZ)

    def test_s3_pitch_one_is_a_wire(self):
        cycle = sine_cycle()
        sampler = mono()
        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER)
        out = render(sampler, 8)
        want = list(cycle) * (8 * BLOCK // CYCLE)
        self.assertEqual(out, want)

    def test_s4_loop_none_stops_and_loop_forever_does_not(self):
        cycle = sine_cycle()
        sampler = mono()
        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_NONE)
        self.assertTrue(sampler.voice[0].playing)
        out = render(sampler, 1)
        self.assertFalse(sampler.voice[0].playing)
        self.assertFalse(sampler.playing)
        # The buffer's frames come out, and nothing after them.
        self.assertEqual(out[:CYCLE], list(cycle))
        self.assertEqual(out[CYCLE:], [0] * (BLOCK - CYCLE))
        self.assertEqual(render(sampler, 1), [0] * BLOCK)

        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER)
        out = render(sampler, 400)
        self.assertTrue(sampler.voice[0].playing)
        self.assertEqual(peak(out[-BLOCK:]), 20000)

    def test_s5_loop_until_release_plays_on_to_the_end(self):
        # Loop [32, 96) of a 160-frame buffer whose tail past the loop is a
        # marker value the loop region never holds.
        marker = 12345
        buffer = sine_cycle(96, peak=8000)
        buffer.extend([marker] * 64)
        sampler = mono()
        sampler.voice[0].play(buffer, loop_start=32, loop_end=96,
                              loop_mode=audiosampler.LOOP_UNTIL_RELEASE,
                              release=0.5)
        held = render(sampler, 40)
        self.assertNotIn(marker, held)
        self.assertTrue(sampler.voice[0].playing)
        sampler.voice[0].release()
        after = render(sampler, 2)
        # The release is half a second, far longer than the 128 frames left,
        # so the tail is heard at nearly full level and then the voice ends.
        self.assertTrue(any(v > marker * 0.9 for v in after))
        self.assertFalse(sampler.voice[0].playing)

    def test_s6_release_fades_and_playing_goes_false(self):
        sampler = mono()
        sampler.voice[0].play(sine_cycle(), loop_mode=audiosampler.LOOP_FOREVER,
                              release=0.1)
        render(sampler, 4)
        sampler.voice[0].release()
        self.assertTrue(sampler.voice[0].playing)
        blocks = int(0.1 * RATE / BLOCK)  # 18.75 -> 18 full blocks
        peaks = [peak(render(sampler, 1)) for _ in range(blocks)]
        for earlier, later in zip(peaks, peaks[1:]):
            self.assertGreater(earlier, later)
        # Linear: the midpoint block sits near half level.
        self.assertAlmostEqual(peaks[blocks // 2] / 20000.0, 0.5, delta=0.06)
        render(sampler, 1)
        self.assertFalse(sampler.voice[0].playing)
        self.assertEqual(render(sampler, 1), [0] * BLOCK)
        # Release time within 2%: the envelope reached zero inside 19 blocks
        # (0.1013 s) and not inside 18 (0.096 s).
        self.assertGreater(peaks[-1], 0)

    def assertFadesOut(self, samples, start, frames=FADE):
        """`samples` falls from `start` to zero within `frames`, never
        rising and never stepping further than a linear ramp over `frames`
        does (plus one LSB of rounding)."""
        bar = -(-start // frames) + 1
        previous = start
        for i, value in enumerate(samples):
            self.assertLessEqual(value, previous, "rose at frame %d" % i)
            self.assertLessEqual(previous - value, bar,
                                 "stepped %d at frame %d, bar %d"
                                 % (previous - value, i, bar))
            previous = value
        self.assertEqual(samples[frames:], [0] * (len(samples) - frames))

    def test_s7_stop_fades_out_within_five_ms(self):
        sampler = mono()
        sampler.voice[0].play(flat(), loop_mode=audiosampler.LOOP_FOREVER,
                              release=1.0)
        self.assertEqual(render(sampler, 2)[-1], 20000)
        sampler.voice[0].stop()
        self.assertFalse(sampler.voice[0].playing)
        self.assertTrue(sampler.playing)  # the fade is still to play
        out = render(sampler, 1)
        self.assertGreater(out[0], 19000)
        self.assertFadesOut(out, 20000)
        self.assertFalse(sampler.playing)
        self.assertEqual(render(sampler, 1), [0] * BLOCK)
        # Sampler.stop() is a voice's stop() on every voice.
        sampler.voice[0].play(flat(), loop_mode=audiosampler.LOOP_FOREVER)
        render(sampler, 1)
        sampler.stop()
        self.assertFadesOut(render(sampler, 1), 20000)
        # deinit() lets go of the buffer, so it cannot fade: it cuts.
        sampler.voice[0].play(flat(), loop_mode=audiosampler.LOOP_FOREVER)
        render(sampler, 1)
        sampler.voice[0].deinit()
        self.assertEqual(render(sampler, 1), [0] * BLOCK)

    def test_s18_a_steal_fades_the_old_note_under_the_new(self):
        sampler = mono()
        sampler.voice[0].play(flat(), loop_mode=audiosampler.LOOP_FOREVER)
        render(sampler, 2)
        sampler.voice[0].play(flat(10000), loop_mode=audiosampler.LOOP_FOREVER)
        self.assertTrue(sampler.voice[0].playing)
        out = render(sampler, 1)
        self.assertFadesOut([v - 10000 for v in out], 20000)
        self.assertEqual(render(sampler, 1), [10000] * BLOCK)
        # A stop and a play in one breath, as an instrument steals: the
        # same fade.
        sampler.voice[0].stop()
        sampler.voice[0].play(flat(5000), loop_mode=audiosampler.LOOP_FOREVER)
        out = render(sampler, 1)
        self.assertFadesOut([v - 5000 for v in out], 10000)

    def test_s19_a_release_keeps_its_pace_but_no_slower_than_the_fade(self):
        sampler = mono()
        # A slow release, stopped: the fade takes over from where it got to.
        sampler.voice[0].play(flat(), loop_mode=audiosampler.LOOP_FOREVER,
                              release=1.0)
        render(sampler, 1)
        sampler.voice[0].release()
        level = render(sampler, 1)[-1]
        self.assertLess(level, 20000)
        sampler.voice[0].stop()
        self.assertFadesOut(render(sampler, 1), level)
        # A release due to end inside the fade ends when it would have.
        sampler.voice[0].play(flat(), loop_mode=audiosampler.LOOP_FOREVER,
                              release=0.001)  # 48 frames
        render(sampler, 1)
        sampler.voice[0].release()
        sampler.voice[0].stop()
        out = render(sampler, 1)
        self.assertGreater(out[0], 19000)
        self.assertFadesOut(out, 20000, frames=48)

    def test_s8_attack_decay_sustain(self):
        sampler = mono()
        # 20 ms attack: block 1 (0-5.3 ms) and block 2 are quieter than 4.
        sampler.voice[0].play(sine_cycle(), loop_mode=audiosampler.LOOP_FOREVER,
                              attack=0.02, hold=0.02, decay=0.1, sustain=0.5)
        first = render(sampler, 1)
        self.assertLess(peak(first), 20000 * 0.3)
        # 1 ms in, the envelope is at 1/20: the sample there is the sine
        # scaled by it, so the block's values are bounded by t/0.02.
        for n, value in enumerate(first):
            bound = 20000 * (n / (0.02 * RATE)) + 1
            self.assertLessEqual(abs(value), bound)
        render(sampler, 3)             # attack complete at 3.75 blocks
        self.assertAlmostEqual(peak(render(sampler, 3)), 20000, delta=20000 * 0.02)
        # Hold ends at 40 ms (7.5 blocks); decay runs to 140 ms (26.25
        # blocks); past that the level is the sustain.
        render(sampler, 22)
        self.assertAlmostEqual(peak(render(sampler, 4)), 10000, delta=10000 * 0.02)

    def test_s9_delay_holds_the_sound_back(self):
        sampler = mono()
        sampler.voice[0].play(sine_cycle(), loop_mode=audiosampler.LOOP_FOREVER,
                              delay=0.01)
        out = render(sampler, 3)
        delay_frames = int(0.01 * RATE)  # 480
        self.assertEqual(out[:delay_frames], [0] * delay_frames)
        # The sound starts at the buffer's start, not 480 frames into it.
        self.assertEqual(out[delay_frames:delay_frames + CYCLE],
                         list(sine_cycle()))

    def test_s10_pan_is_a_linear_balance(self):
        cycle = sine_cycle()
        sampler = audiosampler.Sampler(voice_count=1, channel_count=2,
                                       sample_rate=RATE)
        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER, pan=-1.0)
        out = render(sampler, 2)
        self.assertEqual(channel(out, 0), list(cycle) * (2 * BLOCK // CYCLE))
        self.assertEqual(channel(out, 1), [0] * (2 * BLOCK))
        sampler.voice[0].stop()
        render(sampler, 1)  # let the first note's fade (S18) play out
        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER, pan=0.0,
                              level=0.5)
        out = render(sampler, 2)
        half = [int(v / 2 + (0.5 if v >= 0 else -0.5)) for v in cycle]
        self.assertEqual(channel(out, 0), half * (2 * BLOCK // CYCLE))
        self.assertEqual(channel(out, 1), channel(out, 0))

    def test_s11_level_change_ramps_across_a_block(self):
        # A constant buffer, so the output is the gain itself.
        flat = array("h", [16000] * 64)
        sampler = mono()
        sampler.voice[0].play(flat, loop_mode=audiosampler.LOOP_FOREVER)
        self.assertEqual(render(sampler, 1), [16000] * BLOCK)
        sampler.voice[0].level = 0.0
        ramp = render(sampler, 1)
        self.assertEqual(ramp[0], 16000)
        for earlier, later in zip(ramp, ramp[1:]):
            self.assertGreaterEqual(earlier, later)
        self.assertLess(ramp[-1], 16000 * 2 / BLOCK)
        self.assertEqual(render(sampler, 1), [0] * BLOCK)
        sampler.voice[0].level = 0.25
        render(sampler, 1)
        self.assertEqual(render(sampler, 1), [4000] * BLOCK)

    def test_s12_live_pitch_change(self):
        sampler = mono()
        sampler.voice[0].play(sine_cycle(), loop_mode=audiosampler.LOOP_FOREVER)
        self.assertHz(render(sampler, 40), TONE_HZ)
        sampler.voice[0].pitch = 2.0
        self.assertHz(render(sampler, 40), TONE_HZ * 2)
        sampler.voice[0].pitch = 2.0 ** (-5 / 12.0)
        self.assertHz(render(sampler, 40), TONE_HZ * 2.0 ** (-5 / 12.0))

    def test_s13_voices_sum_and_clamp(self):
        cycle = sine_cycle()
        one = mono(1)
        one.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER)
        two = mono(2)
        two.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER, level=0.5)
        two.voice[1].play(cycle, loop_mode=audiosampler.LOOP_FOREVER, level=0.5)
        a, b = render(one, 4), render(two, 4)
        self.assertEqual(len(a), len(b))
        self.assertLessEqual(max(abs(x - y) for x, y in zip(a, b)), 1)

        loud = mono(4)
        for voice in loud.voice:
            voice.play(cycle, loop_mode=audiosampler.LOOP_FOREVER)
        out = render(loud, 2)
        self.assertEqual(max(out), 32767)
        self.assertEqual(min(out), -32768)
        # Clamped, not wrapped: the sign never flips inside a half cycle.
        for n in range(1, CYCLE // 2):
            self.assertGreaterEqual(out[n], 0)

    def test_s14_bad_loops_and_modes_are_refused(self):
        sampler = mono()
        cycle = sine_cycle()
        with self.assertRaises(ValueError):
            sampler.voice[0].play(cycle, loop_mode=2)
        with self.assertRaises(ValueError):
            sampler.voice[0].play(cycle, loop_start=64, loop_end=32,
                                  loop_mode=audiosampler.LOOP_FOREVER)
        with self.assertRaises(ValueError):
            sampler.voice[0].play(cycle, loop_end=CYCLE + 1,
                                  loop_mode=audiosampler.LOOP_FOREVER)
        with self.assertRaises(ValueError):
            sampler.voice[0].play(array("h"))
        self.assertFalse(sampler.voice[0].playing)
        # A refused play leaves what was sounding sounding.
        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER)
        with self.assertRaises(ValueError):
            sampler.voice[0].play(cycle, loop_mode=2)
        self.assertTrue(sampler.voice[0].playing)
        self.assertEqual(render(sampler, 1), list(cycle) * (BLOCK // CYCLE))
        with self.assertRaises(ValueError):
            audiosampler.Sampler(voice_count=0)
        with self.assertRaises(ValueError):
            audiosampler.Sampler(channel_count=3)
        with self.assertRaises(ValueError):
            audiosampler.Sampler(bits_per_sample=8)
        sampler.deinit()
        with self.assertRaises(Exception):
            sampler.voice[0].play(cycle)

    def test_s15_buffer_is_held_while_sounding(self):
        cycle = sine_cycle()
        sampler = mono()
        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_FOREVER)
        with self.assertRaises(BufferError):
            cycle.append(0)
        sampler.voice[0].stop()
        with self.assertRaises(BufferError):
            cycle.append(0)  # still read by the stop fade
        render(sampler, 1)
        cycle.append(0)  # the fade ended: let go
        sampler.voice[0].play(cycle, loop_mode=audiosampler.LOOP_NONE)
        with self.assertRaises(BufferError):
            cycle.append(0)
        render(sampler, 1)  # the voice ran out
        self.assertFalse(sampler.voice[0].playing)
        cycle.append(0)  # let go without anyone calling stop()

    def test_s16_a_sampler_is_a_mixer_source(self):
        sampler = audiosampler.Sampler(voice_count=1, channel_count=2,
                                       sample_rate=RATE)
        sampler.voice[0].play(sine_cycle(), loop_mode=audiosampler.LOOP_FOREVER)
        mixer = audiomixer.Mixer(voice_count=1, channel_count=2,
                                 sample_rate=RATE, bits_per_sample=16,
                                 samples_signed=True, buffer_size=4 * BLOCK)
        mixer.voice[0].play(sampler)
        out = []
        for _ in range(8):
            result, data = audiocore.get_buffer(mixer)
            out.extend(array("h", bytes(data)))
        self.assertGreater(peak(out), 15000)
        self.assertHz(channel(out, 0), TONE_HZ, rel=0.01)

    def test_s17_twenty_four_voices_inside_the_block(self):
        sampler = audiosampler.Sampler(voice_count=24, channel_count=2,
                                       sample_rate=RATE)
        cycle = sine_cycle()
        for index, voice in enumerate(sampler.voice):
            voice.play(cycle, pitch=2.0 ** ((index - 12) / 12.0),
                       loop_mode=audiosampler.LOOP_FOREVER, level=0.04,
                       pan=(index % 5 - 2) / 2.0, attack=0.01, decay=0.5,
                       sustain=0.5)
        state = sampler._state
        for _ in range(20):
            state.render()
        best = float("inf")
        for _ in range(3):
            started = time.perf_counter()
            for _ in range(500):
                state.render()
            best = min(best, (time.perf_counter() - started) / 500 * 1e6)
        budget = BLOCK / RATE * 1e6  # 5333 us
        print("\n24 looping stereo voices: %.1f us per %d-frame block "
              "(budget %.0f us)" % (best, BLOCK, budget))
        self.assertTrue(all(voice.playing for voice in sampler.voice))
        self.assertLess(best, budget)


    def test_s20_a_hundred_and_twenty_eight_voices_sound_and_sum(self):
        self.assertGreaterEqual(audiosampler.MAX_VOICES, 128)
        sampler = mono(128)
        self.assertEqual(len(sampler.voice), 128)
        for voice in sampler.voice:
            voice.play(flat(100), loop_mode=audiosampler.LOOP_FOREVER)
        self.assertTrue(all(voice.playing for voice in sampler.voice))
        render(sampler, 1)  # a new note's level ramps in across its first block
        self.assertEqual(render(sampler, 1), [128 * 100] * BLOCK)
        sampler.deinit()
        with self.assertRaises(ValueError):
            mono(audiosampler.MAX_VOICES + 1)

    def test_s21_a_bigger_table_renders_the_same_for_the_voices_in_use(self):
        cycle = sine_cycle()
        outs = []
        for table in (64, 128):
            sampler = audiosampler.Sampler(voice_count=table, channel_count=2,
                                           sample_rate=RATE)
            for index in range(64):
                sampler.voice[index].play(
                    cycle, pitch=2.0 ** ((index % 24 - 12) / 12.0),
                    loop_mode=audiosampler.LOOP_FOREVER, level=0.01,
                    pan=(index % 5 - 2) / 2.0, attack=0.01, decay=0.5,
                    sustain=0.5)
            outs.append(render(sampler, 8))
            sampler.deinit()
        self.assertGreater(peak(outs[0]), 1000)
        self.assertEqual(outs[0], outs[1])

    def test_s22_a_steal_on_the_last_voice_fades_the_old_note(self):
        sampler = mono(128)
        for voice in sampler.voice[:127]:
            voice.play(flat(10), loop_mode=audiosampler.LOOP_FOREVER)
        sampler.voice[127].play(flat(), loop_mode=audiosampler.LOOP_FOREVER)
        render(sampler, 2)
        sampler.voice[127].play(flat(10000),
                                loop_mode=audiosampler.LOOP_FOREVER)
        out = render(sampler, 1)
        self.assertFadesOut([v - 127 * 10 - 10000 for v in out], 20000)
        self.assertEqual(render(sampler, 1), [127 * 10 + 10000] * BLOCK)
        sampler.deinit()

    def test_s23_a_hundred_and_twenty_eight_voices_inside_the_block(self):
        sampler = audiosampler.Sampler(voice_count=128, channel_count=2,
                                       sample_rate=RATE)
        cycle = sine_cycle()
        for index, voice in enumerate(sampler.voice):
            voice.play(cycle, pitch=2.0 ** ((index % 24 - 12) / 12.0),
                       loop_mode=audiosampler.LOOP_FOREVER, level=0.01,
                       pan=(index % 5 - 2) / 2.0, attack=0.01, decay=0.5,
                       sustain=0.5)
        state = sampler._state
        for _ in range(20):
            state.render()
        best = float("inf")
        for _ in range(3):
            started = time.perf_counter()
            for _ in range(500):
                state.render()
            best = min(best, (time.perf_counter() - started) / 500 * 1e6)
        budget = BLOCK / RATE * 1e6
        print("\n128 looping stereo voices: %.1f us per %d-frame block "
              "(budget %.0f us)" % (best, BLOCK, budget))
        self.assertTrue(all(voice.playing for voice in sampler.voice))
        self.assertLess(best, budget)


class SampleBufferTraits(unittest.TestCase):
    """Memory for samples: zeroed, writable through a view, and a voice
    playing from a view of it sounds as it does from an array."""

    def test_zeroed_writable_and_sized(self):
        buf = audiosampler.SampleBuffer(64)
        self.assertEqual(len(buf), 64)
        self.assertEqual(bytes(buf), bytes(64))
        view = memoryview(buf)
        view[0:2] = b"\x01\x02"
        self.assertEqual(bytes(buf)[:2], b"\x01\x02")
        del view
        buf.deinit()
        self.assertEqual(len(buf), 0)

    def test_a_voice_plays_from_it_as_from_an_array(self):
        cycle = sine_cycle()
        buf = audiosampler.SampleBuffer(len(cycle) * 2)
        memoryview(buf)[:] = cycle.tobytes()
        outs = []
        for source in (cycle, memoryview(buf).cast("h")):
            sampler = mono()
            sampler.voice[0].play(source, loop_mode=audiosampler.LOOP_FOREVER)
            outs.append(render(sampler, 4))
            sampler.deinit()
        self.assertEqual(outs[0], outs[1])
        self.assertGreater(peak(outs[0]), 10000)

    def test_a_negative_size_is_refused(self):
        with self.assertRaises(ValueError):
            audiosampler.SampleBuffer(-1)


if __name__ == "__main__":
    unittest.main()
