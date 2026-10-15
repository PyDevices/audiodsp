"""Many instruments on many threads, each rendering what it renders alone.

The native extension declares that it doesn't need the GIL, so on a
free-threaded build (3.13t and later) instruments rendering on separate
threads run at the same time. The contract is that no single node is used
from two threads at once; within that, every instrument must render the
same bytes it renders alone.

Each instrument here is a synthesizer with LFOs on its notes, behind a chain
of native nodes (a different chain per instrument, covering every node type
that renders), at one of two sample rates. They are rendered alone first,
then all at once, each on its own thread. On a build with the GIL the threads
take turns and the test still holds them to their solo renders; on a
free-threaded build with the GIL off they run in parallel, and the test also
checks that importing audiodsp left the GIL off.

The last test breaks the contract on purpose: one thread renders a chain
while another reconfigures the same nodes as fast as it can. What comes out
is not defined, but the interpreter must survive it. That runs in a child
interpreter, so a crash fails the test rather than ending the run.
"""

from array import array
import math
import subprocess
import sys
import sysconfig
import textwrap
import threading
import unittest

import audiobiquad
import audioconvolve
import audiocore
import audiodynamics
import audioecho
import audiofilters
import audiofreeverb
import audioladder
import audiomath
import audiomodal
import audioshaper
import audioverb
import synthio

FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))
INSTRUMENTS = 16
BLOCKS = 60
RATES = (48000, 44100)


def gil_enabled():
    probe = getattr(sys, "_is_gil_enabled", None)
    return True if probe is None else probe()


def curve(drive):
    return array("h", (int(32767 * math.tanh(drive * (index / 512 - 1)))
                       for index in range(1025)))


def chain(index, rate, source):
    """The native nodes behind instrument `index`, last one returned."""
    kind = index % 8
    if kind == 0:
        nodes = [audioshaper.Waveshaper(sample_rate=rate, curve=curve(3)),
                 audioverb.Tank(sample_rate=rate, mix=0.4)]
    elif kind == 1:
        nodes = [audioecho.FeedbackDelay(sample_rate=rate, delay_ms=90,
                                         feedback=0.5, mix=0.5,
                                         wow_hz=1.5, wow_depth_ms=2),
                 audioladder.Ladder(sample_rate=rate)]
    elif kind == 2:
        reverb = audioconvolve.Convolver(sample_rate=rate, max_taps=4096,
                                         mix=0.5)
        reverb.synthesize(decay=0.4)
        nodes = [audiodynamics.Dynamics(sample_rate=rate), reverb]
    elif kind == 3:
        nodes = [audiobiquad.Biquad(sample_rate=rate,
                                    frequency=synthio.LFO(rate=0.8,
                                                          scale=900,
                                                          offset=1500)),
                 audiobiquad.AllPass(sample_rate=rate, mix=0.5)]
    elif kind == 4:
        nodes = [audiomodal.Bank(sample_rate=rate)]
    elif kind == 5:
        nodes = [audiomath.SubOctave(sample_rate=rate),
                 audiofreeverb.Freeverb(sample_rate=rate, channel_count=2,
                                        mix=0.4)]
    elif kind == 6:
        nodes = [audiofilters.Filter(
            filter=synthio.Biquad(synthio.FilterMode.LOW_PASS,
                                  frequency=synthio.LFO(rate=0.6, scale=1200,
                                                        offset=2000)),
            sample_rate=rate, channel_count=2, buffer_size=1024)]
    else:
        nodes = [audioshaper.Waveshaper(sample_rate=rate, curve=curve(1.5)),
                 audioecho.FeedbackDelay(sample_rate=rate, delay_ms=40,
                                         feedback=0.3, mix=0.3)]
    for node in nodes:
        node.play(source, loop=True)
        source = node
    return source


def build(index):
    rate = RATES[index % len(RATES)]
    synth = synthio.Synthesizer(sample_rate=rate, channel_count=2)
    vibrato = synthio.LFO(rate=2 + index / 3, scale=0.3)
    synth.blocks.append(vibrato)
    for voice in range(2):
        synth.press(synthio.Note(
            110 * (1 + index / 5) * (1.5 ** voice), bend=vibrato,
            panning=synthio.LFO(rate=0.5 + voice, scale=0.8),
            amplitude=synthio.LFO(rate=3 + index, scale=0.2, offset=0.5)))
    return chain(index, rate, synth)


def render(index):
    sample = build(index)
    return [bytes(audiocore.get_buffer(sample)[1]) for _ in range(BLOCKS)]


class FreeThreadingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.solo = [render(index) for index in range(INSTRUMENTS)]

    @unittest.skipUnless(FREE_THREADED, "not a free-threaded build")
    def test_the_gil_stays_off(self):
        # Importing every audiodsp module above must not have turned it on.
        # Run with -X gil=0, or PYTHON_GIL=0, to hold a build to this.
        if getattr(sys.flags, "gil", None) == 1:
            self.skipTest("the GIL was forced on (-X gil=1)")
        self.assertFalse(gil_enabled(), "importing audiodsp enabled the GIL")

    def test_every_instrument_renders_its_solo_bytes(self):
        renders = [None] * INSTRUMENTS
        failures = []
        start = threading.Barrier(INSTRUMENTS)

        def run(index):
            try:
                sample = build(index)
                start.wait()
                renders[index] = [bytes(audiocore.get_buffer(sample)[1])
                                  for _ in range(BLOCKS)]
            except BaseException as error:  # reported below
                failures.append((index, error))

        interval = sys.getswitchinterval()
        sys.setswitchinterval(1e-6)
        try:
            for _ in range(3):
                threads = [threading.Thread(target=run, args=(index,))
                           for index in range(INSTRUMENTS)]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join(300)
                self.assertEqual(failures, [])
                differing = [index for index in range(INSTRUMENTS)
                             if renders[index] != self.solo[index]]
                self.assertEqual(differing, [],
                                 "instruments that differ from their solo")
        finally:
            sys.setswitchinterval(interval)


RECONFIGURE = textwrap.dedent("""
    import math, sys, threading
    from array import array
    import audiocore, audioconvolve, audioshaper, audioverb, synthio

    synth = synthio.Synthesizer(sample_rate=48000, channel_count=2)
    synth.press(synthio.Note(220))
    def curve(drive):
        return array("h", (int(32767 * math.tanh(drive * (i / 512 - 1)))
                           for i in range(1025)))
    shaper = audioshaper.Waveshaper(curve=curve(2))
    room = audioconvolve.Convolver(max_taps=8192, mix=0.5)
    room.synthesize(decay=0.3)
    tank = audioverb.Tank(mix=0.4)
    shaper.play(synth, loop=True)
    room.play(shaper, loop=True)
    tank.play(room, loop=True)
    stop = threading.Event()

    def render():
        while not stop.is_set():
            try:
                audiocore.get_buffer(tank)
            except Exception:
                pass

    def reconfigure():
        for turn in range(400):
            for call in (
                lambda: shaper._state.load_curve(
                    bytes(memoryview(curve(1 + turn % 5)).cast("B"))),
                lambda: room.synthesize(decay=0.1 + (turn % 7) / 10),
                lambda: tank.set(delays=[300 + 37 * line + turn % 50
                                         for line in range(12)]),
                lambda: tank._state.__init__(sample_rate=48000),
                lambda: shaper._state.__init__(sample_rate=48000),
            ):
                try:
                    call()
                except Exception:
                    pass
        stop.set()

    threads = [threading.Thread(target=render),
               threading.Thread(target=render),
               threading.Thread(target=reconfigure)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    print("survived")
""")


class ReconfigureWhileRenderingTest(unittest.TestCase):
    def test_the_interpreter_survives(self):
        result = subprocess.run([sys.executable, "-c", RECONFIGURE],
                                capture_output=True, text=True, timeout=600)
        self.assertEqual(
            (result.returncode, result.stdout.strip()), (0, "survived"),
            "child interpreter: %s" % result.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
