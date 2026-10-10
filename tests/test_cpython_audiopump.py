"""audiopump on CPython: the service-mode engine, as audiodev drives it.

The byte-identity, storm, ring, queue and tap gates are tests/pump/*_probe.py,
which run here and on MicroPython alike. These pin the lifecycle a driver
relies on: what spawn() returns, when service() gives the thread back, and
that every byte pulled is drained.
"""

import struct
import unittest
from array import array

import audiocore
import audiomixer
import audiopump

RATE = 48000
FRAMES = 256


def status():
    return bytearray(audiopump.STATUS_BYTES)


def words(block):
    return struct.unpack("<%dQ" % audiopump.STATUS_WORDS, block)


def tone(frames=FRAMES * 8, step=97):
    data = array("h", bytes(4 * frames))
    for index in range(frames):
        data[2 * index] = data[2 * index + 1] = ((index * step) % 4096) - 2048
    return data


def graph():
    """A Mixer looping a RawSample: 1024-byte blocks, for ever."""
    mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE, channel_count=2,
                             bits_per_sample=16, samples_signed=True,
                             buffer_size=FRAMES * 4 * 2)
    source = audiocore.RawSample(tone(), sample_rate=RATE, channel_count=2)
    mixer.play(source, voice=0, loop=True)
    return mixer, source


def fnv(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


class Base(unittest.TestCase):
    def tearDown(self):
        audiopump.shutdown()


class TheShapeOfThisPort(Base):
    def test_no_thread_and_no_driver(self):
        self.assertEqual(audiopump.driver(), "none")
        self.assertFalse(audiopump.threaded())
        self.assertTrue(audiopump.backpressure())

    def test_spawn_adopts_and_pulls_nothing(self):
        mixer, _source = graph()
        block = status()
        self.assertEqual(audiopump.spawn(mixer, 100, block,
                                         ring=bytearray(8192)), -2)
        self.assertTrue(audiopump.running())
        self.assertEqual(words(block)[0], 0)
        self.assertEqual(audiopump.now(), 0)

    def test_a_second_spawn_is_refused_until_shutdown(self):
        mixer, _source = graph()
        audiopump.spawn(mixer, 10, status(), ring=bytearray(8192))
        with self.assertRaises(ValueError):
            audiopump.spawn(mixer, 10, status(), ring=bytearray(8192))
        audiopump.shutdown()
        audiopump.spawn(mixer, 10, status(), ring=bytearray(8192))

    def test_info_reads_the_block_size(self):
        source = audiocore.RawSample(tone(), sample_rate=RATE, channel_count=2)
        self.assertEqual(audiopump.info(source),
                         (RATE, 2, 16, FRAMES * 8 * 4, True, True))


class ServiceFillsTheRingAndStops(Base):
    def test_full_ring_gives_the_thread_back(self):
        mixer, _source = graph()
        audiopump.spawn(mixer, 1000, status(), ring=bytearray(4096))
        packed = audiopump.service()
        self.assertEqual(packed & audiopump.SERVICE_MASK,
                         audiopump.SERVICE_FULL)
        # Three, not four: the room reserved is the Mixer's declared maximum,
        # its buffer_size of 2048, though it hands back 1024 a pull. The
        # native pump reserves the same.
        self.assertEqual(packed >> audiopump.SERVICE_SHIFT, 3)
        # Nothing is dropped: the next call pulls nothing until room appears.
        self.assertEqual(audiopump.service() >> audiopump.SERVICE_SHIFT, 0)
        self.assertEqual(audiopump.drain(bytearray(1024)), 1024)
        self.assertEqual(audiopump.service() >> audiopump.SERVICE_SHIFT, 1)

    def test_a_budget_caps_one_call(self):
        mixer, _source = graph()
        audiopump.spawn(mixer, 1000, status(), ring=bytearray(65536))
        packed = audiopump.service(3)
        self.assertEqual(packed & audiopump.SERVICE_MASK,
                         audiopump.SERVICE_MORE)
        self.assertEqual(packed >> audiopump.SERVICE_SHIFT, 3)

    def test_every_byte_pulled_is_drained_in_order(self):
        blocks = 40
        mixer, _source = graph()
        here = status()
        audiopump.pull(mixer, blocks, here)

        mixer, _source = graph()
        block = status()
        audiopump.spawn(mixer, blocks, block, ring=bytearray(3000))
        out = bytearray()
        buf = bytearray(700)
        while True:
            why = audiopump.service() & audiopump.SERVICE_MASK
            got = audiopump.drain(buf)
            out += buf[:got]
            if why == audiopump.SERVICE_DONE and not got:
                break
        w = words(block)
        self.assertEqual(w[0], blocks)
        self.assertEqual(len(out), blocks * 1024)
        self.assertEqual(w[2], words(here)[2])        # the pull's digest
        self.assertEqual(w[11], fnv(out))             # what was drained
        self.assertEqual(w[2], w[11])
        self.assertEqual(w[10], 0)                    # nothing dropped
        self.assertEqual(audiopump.now(), blocks * FRAMES)
        self.assertTrue(audiopump.join())
        self.assertFalse(audiopump.running())


class ParkStopAndRetarget(Base):
    def test_park_holds_at_a_block_boundary(self):
        mixer, _source = graph()
        block = status()
        audiopump.spawn(mixer, 1000, block, ring=bytearray(65536))
        audiopump.service(2)
        self.assertTrue(audiopump.park())
        packed = audiopump.service()
        self.assertEqual(packed, audiopump.SERVICE_PARKED)
        self.assertEqual(words(block)[7], 1)
        audiopump.unpark()
        self.assertEqual(audiopump.service(2) >> audiopump.SERVICE_SHIFT, 2)
        self.assertEqual(words(block)[7], 0)
        self.assertEqual(words(block)[17], 1)

    def test_stop_ends_the_loop_at_the_next_service(self):
        mixer, _source = graph()
        audiopump.spawn(mixer, 1000, status(), ring=bytearray(65536))
        audiopump.service(2)
        self.assertFalse(audiopump.join())
        audiopump.stop()
        self.assertEqual(audiopump.service() & audiopump.SERVICE_MASK,
                         audiopump.SERVICE_DONE)
        self.assertTrue(audiopump.join())

    def test_a_tail_that_ends_says_so(self):
        source = audiocore.RawSample(tone(FRAMES), sample_rate=RATE,
                                     channel_count=2)
        block = status()
        audiopump.spawn(source, 1000, block, ring=bytearray(65536))
        self.assertEqual(audiopump.service() & audiopump.SERVICE_MASK,
                         audiopump.SERVICE_DONE)
        self.assertEqual(words(block)[5], 3)          # the source ran out
        self.assertFalse(audiopump.running())

    def test_retarget_carries_the_loop_flag(self):
        mixer, _source = graph()
        audiopump.spawn(mixer, 1000, status(), ring=bytearray(65536))
        audiopump.service(2)
        looped = audiocore.RawSample(tone(FRAMES), sample_rate=RATE,
                                     channel_count=2)
        audiopump.retarget(looped, loop=True)
        self.assertEqual(audiopump.service(10) >> audiopump.SERVICE_SHIFT, 10)
        self.assertTrue(audiopump.running())

    def test_a_fading_retarget_ramps_out_then_in(self):
        # The native pump's ramps, to the sample: the old tail's next block
        # down to silence, then the new tail's first block up from it.
        def flat(level, frames=256):
            return audiocore.RawSample(array("h", [level] * frames * 2),
                                       sample_rate=RATE, channel_count=2)

        def ramp(level, n, up):
            return [(level * (((k + 1) << 15) // n if up else
                              (32768 * (n - 1 - k)) // n)) >> 15
                    for k in range(n)]

        old, new = flat(8000), flat(-6000)
        audiopump.spawn(old, 1000, status(), ring=bytearray(65536), loop=True)
        audiopump.service(2)
        self.assertFalse(audiopump.retarget(new, loop=True, fade=True))
        audiopump.service(4)
        out = bytearray(65536)
        got = array("h", bytes(out[:audiopump.drain(out)]))
        frames = list(got[::2])
        self.assertEqual(frames[:512], [8000] * 512)
        self.assertEqual(frames[512:1024],
                         ramp(8000, 256, False) + ramp(-6000, 256, True))
        self.assertEqual(frames[1024:], [-6000] * (len(frames) - 1024))
        self.assertEqual(audiopump.fades(), (1, 0, 0))

    def test_a_released_tail_is_a_fault_not_an_exception(self):
        mixer, _source = graph()
        block = status()
        audiopump.spawn(mixer, 1000, block, ring=bytearray(65536))
        audiopump.service(2)
        mixer.deinit()
        self.assertEqual(audiopump.service() & audiopump.SERVICE_MASK,
                         audiopump.SERVICE_DONE)
        self.assertEqual(words(block)[24], 2)
        self.assertEqual(audiopump.fault(), 2)

    def test_shutdown_lets_go_of_the_queue_and_the_tap(self):
        audiopump.events(audiopump.Events(capacity=4))
        audiopump.tap(audiopump.Tap(frames=64))
        audiopump.shutdown()
        self.assertIsNone(audiopump.events())
        self.assertIsNone(audiopump.tap())


if __name__ == "__main__":
    unittest.main()
