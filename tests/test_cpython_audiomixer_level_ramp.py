# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""`Mixer.level_ramp`: a voice's level move walks instead of stepping.

CircuitPython's Mixer holds a level move until the voice's next zero crossing,
so it cannot step a single waveform mid-cycle. Two voices crossfading (a dry
path and a wet one) cross zero at different moments, so their sum still
steps. With `level_ramp` set to a number of frames, a move walks to its new
level in a straight line over that many frames, one step a frame in a stereo
mixer and one every two frames in a mono one (a packed word), and lands
exactly on the level asked for. `src/audiomixer/Mixer.c` is the authority; the
CPython twin mirrors it.

| what it checks | bar |
|---|---|
| the default is CircuitPython's | `level_ramp` is 0, and a Mixer set to 0 renders the bytes of one never touched, through a level move |
| a voice's first level is not walked | the first block at `level_ramp` 960 is byte-identical to the same block at 0 |
| the walk is a straight line and lands | after a move from 1.0 to 0.25 on DC, the level falls by the same step, to within one LSB, frame after frame, and is exactly 0.25's product from frame 960 on |
| a mono mixer steps once a word | at `level_ramp` 960 a mono voice walks in 480 steps, each held for two frames |
| a walk spans blocks | 256-frame blocks give the same frames as 2048-frame blocks |
| a move during a walk starts from where it stands | no frame steps by more than one step's worth, and it lands on the second target 960 frames after the second move |
| a pan move walks too | a stereo voice panned hard left in one call walks its right channel down |
| the crossfade that clicks without it | a sine voice going 1.0 to 0.0 while a voice that never crosses zero goes 0.0 to 1.0: the largest step within the move is under 1.5 times the tone's own at `level_ramp` 960, and over it at 0 |
| out of range is refused | -1 and 2**24 + 1 raise ValueError |

`tests/parity/mixer_level_ramp_probe.py` holds MicroPython's native mixer to
the same bytes. `verify_dsp.py` runs it there and here, and not on
CircuitPython, whose Mixer has no `level_ramp`.
"""

import array
import math
import unittest

import audiocore
import audiomixer

RATE = 48000
RAMP = 960
DC = 16384


def _mixer(channels=2, voices=1, buffer_frames=256):
    return audiomixer.Mixer(
        voice_count=voices, sample_rate=RATE, channel_count=channels,
        bits_per_sample=16, samples_signed=True,
        buffer_size=buffer_frames * channels * 2 * 2)


def _source(values, channels):
    return audiocore.RawSample(array.array("h", values), sample_rate=RATE,
                               channel_count=channels)


def _pull(mixer, frames, channels):
    """`frames` frames, pulled whole blocks at a time: a multiple of the
    block, or the rest of the last block is lost."""
    out = array.array("h")
    while len(out) < frames * channels:
        out.frombytes(bytes(audiocore.get_buffer(mixer)[1]))
    return list(out[:frames * channels])


def _sine(frames, phase=0.0):
    return [int(12000 * math.sin(2 * math.pi * 200.0 * i / RATE + phase))
            for i in range(frames)]


def _stereo(values):
    return [v for v in values for _ in (0, 1)]


def _dc_move(ramp, channels=2, buffer_frames=256, to=0.25, pan=None):
    """A DC voice at 1.0 for 512 frames, then a move of level or pan."""
    mixer = _mixer(channels, buffer_frames=buffer_frames)
    mixer.level_ramp = ramp
    mixer.voice[0].level = 1.0
    mixer.play(_source([DC] * (8192 * channels), channels), loop=True)
    head = _pull(mixer, 512, channels)
    if pan is None:
        mixer.voice[0].level = to
    else:
        mixer.voice[0].panning = pan
    return head, _pull(mixer, 2048, channels)


class LevelRamp(unittest.TestCase):

    def test_default_is_circuitpythons(self):
        self.assertEqual(_mixer().level_ramp, 0)
        renders = []
        for touch in (False, True):
            mixer = _mixer()
            if touch:
                mixer.level_ramp = 0
            mixer.play(_source(_stereo(_sine(4800)), 2), loop=True)
            first = _pull(mixer, 512, 2)
            mixer.voice[0].level = 0.3
            renders.append(first + _pull(mixer, 1024, 2))
        self.assertEqual(renders[0], renders[1])

    def test_first_level_is_not_walked(self):
        blocks = []
        for ramp in (0, RAMP):
            mixer = _mixer()
            mixer.level_ramp = ramp
            mixer.voice[0].level = 0.5
            mixer.play(_source(_stereo(_sine(4800)), 2), loop=True)
            blocks.append(_pull(mixer, 256, 2))
        self.assertEqual(blocks[0], blocks[1])

    def test_walk_is_a_straight_line_and_lands(self):
        head, tail = _dc_move(RAMP)
        # The first stretch waits for a zero crossing DC never makes, as
        # CircuitPython's does; the level has landed by the second.
        self.assertEqual(set(head[256 * 2:]), {DC})
        left = tail[0::2]
        target = (DC * 8192) >> 15
        steps = [left[i - 1] - left[i] for i in range(1, RAMP)]
        self.assertLessEqual(max(steps) - min(steps), 1, steps[:8])
        self.assertTrue(all(v > target for v in left[:RAMP - 1]))
        self.assertEqual(left[RAMP - 1:], [target] * (len(left) - RAMP + 1))
        self.assertEqual(tail[0::2], tail[1::2])

    def test_mono_steps_once_a_word(self):
        _, tail = _dc_move(RAMP, channels=1)
        target = (DC * 8192) >> 15
        pairs = [(tail[i], tail[i + 1]) for i in range(0, RAMP, 2)]
        self.assertTrue(all(a == b for a, b in pairs))
        self.assertTrue(all(a > target for a, _ in pairs[:-1]))
        self.assertEqual(tail[RAMP - 2:], [target] * (len(tail) - RAMP + 2))

    def test_walk_spans_blocks(self):
        _, small = _dc_move(RAMP, buffer_frames=256)
        _, large = _dc_move(RAMP, buffer_frames=2048)
        self.assertEqual(small, large)

    def test_move_during_walk(self):
        mixer = _mixer()
        mixer.level_ramp = RAMP
        mixer.voice[0].level = 1.0
        mixer.play(_source([DC] * 16384, 2), loop=True)
        _pull(mixer, 512, 2)
        mixer.voice[0].level = 0.0
        first = _pull(mixer, 512, 2)[0::2]
        mixer.voice[0].level = 1.0
        second = _pull(mixer, 2048, 2)[0::2]
        trace = first + second
        biggest = max(abs(trace[i] - trace[i - 1])
                      for i in range(1, len(trace)))
        self.assertLessEqual(biggest, DC // RAMP + 1)
        self.assertEqual(second[RAMP - 1:], [DC] * (len(second) - RAMP + 1))
        self.assertLess(second[RAMP - 2], DC)

    def test_pan_walks(self):
        _, tail = _dc_move(RAMP, to=None, pan=-1.0)
        left, right = tail[0::2], tail[1::2]
        self.assertEqual(set(left), {DC})
        self.assertLess(right[RAMP // 2], DC)
        self.assertGreater(right[RAMP // 2], 1)
        self.assertEqual(right[RAMP:], [right[RAMP]] * (len(right) - RAMP))
        self.assertLessEqual(right[RAMP], 1)

    def _crossfade_step(self, ramp):
        """The largest step within a crossfade, over the tone's own."""
        mixer = _mixer(voices=2)
        mixer.level_ramp = ramp
        mixer.voice[0].level = 1.0
        mixer.voice[1].level = 0.0
        mixer.voice[0].play(_source(_stereo(_sine(4800)), 2), loop=True)
        # The wet voice never crosses zero, as a shaper's lopsided output
        # may not, so without the walk its move waits for the end of a
        # stretch and lands there at once.
        wet = [7000 + v // 4 for v in _sine(4800, math.pi / 2)]
        mixer.voice[1].play(_source(_stereo(wet), 2), loop=True)
        head = _pull(mixer, 2048, 2)[0::2]
        mixer.voice[0].level = 0.0
        mixer.voice[1].level = 1.0
        tail = _pull(mixer, 4096, 2)[0::2]
        trace = head + tail
        at = len(head)
        own = max(abs(trace[i] - trace[i - 1]) for i in range(at - 1024, at))
        near = max(abs(trace[i] - trace[i - 1]) for i in range(at, at + 2048))
        return near / own

    def test_crossfade_does_not_click(self):
        self.assertLess(self._crossfade_step(RAMP), 1.5)
        self.assertGreater(self._crossfade_step(0), 1.5)

    def test_out_of_range_is_refused(self):
        mixer = _mixer()
        for bad in (-1, (1 << 24) + 1):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    mixer.level_ramp = bad
        mixer.level_ramp = 1 << 24
        self.assertEqual(mixer.level_ramp, 1 << 24)


if __name__ == "__main__":
    unittest.main()
