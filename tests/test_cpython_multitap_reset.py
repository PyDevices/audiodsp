"""MultiTapDelay: a reset node is a fresh node.

`audiocore.reset_buffer` on a MultiTapDelay emptied the line but left the
write head where the pulls had taken it, while a fresh node's starts at the
top. A Time shortened afterwards keeps the line up to its new length and
starts the head again at the top when it sits past it, so which stretch of
the line survives the move depended on how long the node had played before
its reset: after a reset, a Time move played repeats a fresh node did not
(an extra full-level echo in audiocomponents' MultiTapDelay). The reset now
puts both lanes' heads at the top, as construction does.

CircuitPython builds upstream's own MultiTapDelay, which keeps the head; the
departure is recorded in docs/upstream-diff.md.
"""

import unittest
from array import array

import audiocore
import audiodelays


def material(rate, channels, frames):
    values = array("h")
    state = 12345
    for frame in range(frames):
        for _channel in range(channels):
            state = (state * 1103515245 + 12345) & 0x7fffffff
            values.append(((state >> 8) % 20001) - 10000)
    return audiocore.RawSample(values, sample_rate=rate,
                               channel_count=channels)


def node(rate, channels):
    return audiodelays.MultiTapDelay(
        max_delay_ms=400, delay_ms=300, decay=0.0, mix=1.0,
        taps=((1.0, 1.0), (0.5, 0.7)), buffer_size=512 * channels,
        sample_rate=rate, channel_count=channels)


def pull(n, count):
    out = bytearray()
    for _ in range(count):
        out.extend(bytes(audiocore.get_buffer(n)[1]))
    return bytes(out)


class AResetNodeIsAFreshNode(unittest.TestCase):

    def run_case(self, rate, channels, played, short_ms):
        frames = rate * 3
        used = node(rate, channels)
        used.play(material(rate, channels, frames), loop=True)
        pull(used, played)
        audiocore.reset_buffer(used)
        used.play(material(rate, channels, frames), loop=True)
        fresh = node(rate, channels)
        fresh.play(material(rate, channels, frames), loop=True)
        a = pull(used, 4)
        b = pull(fresh, 4)
        self.assertEqual(a, b, "before the move")
        used.delay_ms = short_ms
        fresh.delay_ms = short_ms
        a = pull(used, 40)
        b = pull(fresh, 40)
        used.deinit()
        fresh.deinit()
        return a == b

    def test_a_shorter_time_after_a_reset_plays_as_on_a_fresh_node(self):
        for rate in (48000, 22050):
            for channels in (1, 2):
                for played in (9, 37):
                    self.assertTrue(
                        self.run_case(rate, channels, played, 20.0),
                        (rate, channels, played))


if __name__ == "__main__":
    unittest.main()
