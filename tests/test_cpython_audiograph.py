"""audiograph on CPython: the board's module, with the board's names.

| trait | bar |
|---|---|
| `graph_probe.py`, the runner's own probe, passes here | GRAPH PASS |
| every name the MicroPython module exports exists here | the same set |
| a wow table handed to a delay cannot be resized under it | BufferError |
| a graph refuses a second `__init__` | RuntimeError |
| a graph built and dropped many times frees what it took | no growth |

The renders themselves are held to MicroPython's and CircuitPython's by
`tests/parity/verify_graph.py`, sample for sample; this file is what the wheel
alone can check.
"""

from array import array
import pathlib
import re
import subprocess
import sys
import tracemalloc
import unittest

import audiograph as ag

ROOT = pathlib.Path(__file__).resolve().parent.parent


class TheModule(unittest.TestCase):

    def test_the_runner_passes_its_own_probe(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "parity" / "graph_probe.py")],
            capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("GRAPH PASS", result.stdout)

    def test_every_name_the_board_exports(self):
        table = (ROOT / "src" / "audiograph" / "module.c").read_text()
        names = set(re.findall(r"MP_ROM_QSTR\(MP_QSTR_(\w+)\)", table))
        names -= {"__name__", "audiograph"}
        missing = sorted(n for n in names if not hasattr(ag, n))
        self.assertEqual(missing, [])
        self.assertIsInstance(ag.Graph(), ag.Graph)

    def test_a_borrowed_table_is_held(self):
        g = ag.Graph(buffers=2, nodes=2)
        src, dst = g.buffer(), g.buffer()
        d = g.add(ag.FBDELAY, ag.ALWAYS, (src,), dst, 10.0)
        wow = array("h", [0, 100, 0, -100])
        g.table(d, wow)
        with self.assertRaises(BufferError):
            wow.append(0)
        with self.assertRaises(TypeError):
            g.table(d, bytearray(8))
        g.run(None)

    def test_init_twice_is_refused(self):
        g = ag.Graph()
        with self.assertRaises(RuntimeError):
            g.__init__()

    def test_keyword_only_like_the_board(self):
        with self.assertRaises(TypeError):
            ag.Graph(48000)

    def test_a_dropped_graph_frees_its_memory(self):
        def build():
            g = ag.Graph(buffers=16, nodes=16)
            bufs = [g.buffer() for _ in range(8)]
            g.add(ag.OSC, 0, (None, None, None, None), bufs[0])
            g.add(ag.LADDER, 0, (bufs[0], None), bufs[1])
            g.add(ag.FBDELAY, ag.ALWAYS, (bufs[1],), bufs[2], 100.0)
            g.add(ag.OUT, ag.ALWAYS, (bufs[2], bufs[2]), None)
            g.run(bytearray(128 * 4))

        build()
        tracemalloc.start()
        try:
            base = tracemalloc.get_traced_memory()[0]
            for _ in range(200):
                build()
            grown = tracemalloc.get_traced_memory()[0] - base
        finally:
            tracemalloc.stop()
        # One graph with a 100 ms line is tens of kilobytes; 200 kept would be
        # megabytes.
        self.assertLess(grown, 64 * 1024)


if __name__ == "__main__":
    unittest.main()
