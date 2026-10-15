"""synthio's block clock belongs to the render in progress.

Every render (a Synthesizer's block, an effect's block) advances the block
clock, and every LFO and Math block reads it to know how far to step. When
that clock was one value for the whole process, a synthesizer rendering on
one thread stepped the LFOs of a synthesizer rendering on another: each
render came out different from the same instrument rendered alone.

The rule these tests hold: an instrument (a synthesizer with LFOs on its
blocks, its notes and its filters, behind an effect whose filter is swept by
another LFO) renders the same bytes whether it runs alone, interleaved block
by block with other instruments on one thread, or on its own thread beside
them. Each instrument uses a different sample rate, so a clock leaking from
one into another changes how far its LFOs step and cannot go unseen.
"""

import sys
import threading
import unittest

import audiocore
import audiofilters
import synthio

RATES = (48000, 22050, 44100, 11025, 32000, 16000)
INSTRUMENTS = 6
BLOCKS = 120


def build(index):
    """One instrument: returns the sample to pull blocks from."""
    rate = RATES[index % len(RATES)]
    synth = synthio.Synthesizer(sample_rate=rate, channel_count=1)
    vibrato = synthio.LFO(rate=3 + index, scale=0.5)
    tremolo = synthio.LFO(rate=1.5 + index, scale=0.3, offset=0.6)
    synth.blocks.append(vibrato)
    cutoff = synthio.Math(synthio.MathOperation.SCALE_OFFSET,
                          synthio.LFO(rate=0.7 + index / 3), 800, 1600)
    note = synthio.Note(220 * (1 + index / 7), bend=vibrato,
                        amplitude=tremolo,
                        filter=synthio.Biquad(synthio.FilterMode.LOW_PASS,
                                              frequency=cutoff))
    synth.press(note)
    sweep = synthio.LFO(rate=0.5 + index / 5, scale=1500, offset=2500)
    effect = audiofilters.Filter(
        filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, frequency=sweep),
        sample_rate=rate, channel_count=1, buffer_size=512)
    effect.play(synth, loop=True)
    return effect


def pull(sample):
    return bytes(audiocore.get_buffer(sample)[1])


def solo(index):
    sample = build(index)
    return [pull(sample) for _ in range(BLOCKS)]


class BlockClockTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.solo = [solo(index) for index in range(INSTRUMENTS)]

    def assertMatchesSolo(self, renders):
        differing = [index for index, blocks in enumerate(renders)
                     if blocks != self.solo[index]]
        self.assertEqual(differing, [],
                         "instruments that differ from their solo render")

    def test_solo_is_repeatable(self):
        self.assertMatchesSolo([solo(index) for index in range(INSTRUMENTS)])

    def test_interleaved_on_one_thread(self):
        samples = [build(index) for index in range(INSTRUMENTS)]
        renders = [[] for _ in samples]
        for _ in range(BLOCKS):
            for blocks, sample in zip(renders, samples):
                # Building an LFO between renders must not disturb anyone.
                synthio.LFO(rate=9)
                blocks.append(pull(sample))
        self.assertMatchesSolo(renders)

    def test_each_on_its_own_thread(self):
        renders = [None] * INSTRUMENTS
        failures = []
        start = threading.Barrier(INSTRUMENTS)

        def run(index):
            try:
                sample = build(index)
                start.wait()
                renders[index] = [pull(sample) for _ in range(BLOCKS)]
            except BaseException as error:  # reported below, not swallowed
                failures.append(error)

        # Switch threads as often as the interpreter allows, so renders
        # overlap on a build with the GIL as well as on one without it.
        interval = sys.getswitchinterval()
        sys.setswitchinterval(1e-6)
        try:
            threads = [threading.Thread(target=run, args=(index,))
                       for index in range(INSTRUMENTS)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(120)
        finally:
            sys.setswitchinterval(interval)
        self.assertEqual(failures, [])
        self.assertMatchesSolo(renders)

    def test_threads_hand_off(self):
        # An instrument built on one thread and rendered a block at a time on
        # fresh threads, then the rest on one more: its LFOs keep stepping
        # once per render, however new the thread that renders it.
        sample = build(1)
        blocks = []

        def run(count):
            blocks.extend(pull(sample) for _ in range(count))

        for count in (1, 1, 1, BLOCKS - 3):
            thread = threading.Thread(target=run, args=(count,))
            thread.start()
            thread.join(60)
        self.assertEqual(blocks, self.solo[1])


if __name__ == "__main__":
    unittest.main()
