"""A native sample holding a memoryview must survive the cyclic collector.

`RawSample` and `BufferOwner` keep a `Py_buffer` open on whatever buffer they
were given. When that is a `memoryview` and both sit in an unreachable cycle,
the collector may clear the memoryview first. A memoryview with an export
cannot release itself, so it drops its managed buffer anyway, and the
sample's own release then runs against a dead view: CPython segfaulted inside
garbage collection. audioeffects.Vibrato hit it when its source ran out.

The crash kills the interpreter, so each case runs in a child process.
"""

import subprocess
import sys
import textwrap
import unittest

CASE = textwrap.dedent("""
    import gc
    import _audiodsp
    import audiocore
    for make in ({make}):
        for _ in range(20):
            view = memoryview(bytearray(64))
            node = make(view)
            cycle = [view, node]
            cycle.append(cycle)
            del view, node, cycle
            gc.collect()
    print("ok")
""")


class BufferHolderCollectionTests(unittest.TestCase):
    def _run(self, make):
        result = subprocess.run(
            [sys.executable, "-X", "faulthandler", "-c",
             CASE.format(make=make)],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        self.assertEqual(result.stdout.strip(), "ok")
        self.assertNotIn("BufferError", result.stderr)

    def test_rawsample_over_memoryview_in_a_cycle(self):
        self._run("lambda v: audiocore.RawSample(v, sample_rate=8000),")

    def test_buffer_owner_over_memoryview_in_a_cycle(self):
        self._run("_audiodsp.BufferOwner,")

    def test_released_sample_still_frees_its_buffer(self):
        # Not visiting the exporter must not keep it alive past the holder.
        import gc
        import weakref
        import audiocore

        class Exporter(bytearray):
            pass

        data = Exporter(64)
        ref = weakref.ref(data)
        node = audiocore.RawSample(memoryview(data), sample_rate=8000)
        del data
        self.assertIsNotNone(ref())
        del node
        gc.collect()
        self.assertIsNone(ref())


if __name__ == "__main__":
    unittest.main()
