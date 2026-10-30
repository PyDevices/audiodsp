"""audiograph on CPython: the board's module, with the board's names.

| trait | bar |
|---|---|
| `graph_probe.py`, the runner's own probe, passes here | GRAPH PASS |
| every name the MicroPython module exports exists here | the same set |
| a wow table handed to a delay cannot be resized under it | BufferError |
| a graph refuses a second `__init__` | RuntimeError |
| a graph built and dropped many times frees what it took | no growth |
| `graph_source_probe.py`: pulled as a source, it renders what `run()` does | GRAPH SOURCE PASS |
| every method the MicroPython `Graph` has exists here | the same set |
| `hold()` keeps a batch back until `commit()`, and `get()` reads the last `set()` | exact |
| a queue too small for a batch splits it and still applies all of it | counted |
| `graph_carry_probe.py`: a graph that takes a running one's nodes renders what it would have | GRAPH CARRY PASS |
| `carry()` refuses a node of another kind, arg, state size or sample rate, a bad node and a non-graph | ValueError, TypeError |
| a wow table a carried delay borrowed from its source outlives the source | held |
| a chain of graphs, each carrying many nodes from the last, holds each table once | bounded |
| `swap()` refuses a non-graph and a graph of another rate, block or channel count | TypeError, ValueError |
| a graph built, carried and swapped in mid-stream plays on as if nothing changed | exact |
| `swap()` trades the patches and keeps the frame count with the stream | exact |

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

    def test_pulled_as_a_source_it_renders_what_run_does(self):
        result = subprocess.run(
            [sys.executable,
             str(ROOT / "tests" / "parity" / "graph_source_probe.py")],
            capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("GRAPH SOURCE PASS", result.stdout)

    def test_every_method_the_board_has(self):
        table = (ROOT / "src" / "audiograph" / "Graph.c").read_text()
        names = set(re.findall(r"MP_ROM_QSTR\(MP_QSTR_(\w+)\)", table))
        missing = sorted(n for n in names if not hasattr(ag.Graph, n))
        self.assertEqual(missing, [])

    def _pair(self, **kw):
        g = ag.Graph(frames=4, channel_count=2, buffers=2, nodes=3, **kw)
        left, right = g.buffer(), g.buffer()
        a = g.add(ag.CONST, ag.ALWAYS, (), left)
        b = g.add(ag.CONST, ag.ALWAYS, (), right)
        g.add(ag.OUT, ag.ALWAYS, (left, right), None)
        return g, a, b

    @staticmethod
    def _pull(g):
        return list(array("h", bytes(g._get_buffer(False, 0)[1])))

    def test_hold_keeps_a_batch_back_until_commit(self):
        g, a, b = self._pair()
        g.hold()
        g.set(a, 0, 100.0)
        g.set(b, 0, -100.0)
        self.assertEqual(g.get(a, 0), 100.0)      # the shadow: already set
        self.assertEqual(self._pull(g), [0] * 8)  # but not yet heard
        g.commit()
        self.assertEqual(self._pull(g), [100, -100] * 4)
        sounding, frame, applied, pushed = g.sounding()
        self.assertEqual((frame, applied, pushed), (8, 2, 2))

    def test_a_small_queue_splits_a_batch_and_keeps_it_whole(self):
        g, a, b = self._pair(queue=2)
        g.hold()
        for v in range(1, 6):
            g.set(a, 0, float(v))
            g.set(b, 0, float(-v))
        g.commit()
        self.assertEqual(self._pull(g), [5, -5] * 4)
        stats = g.stats()
        self.assertEqual(stats["capacity"], 2)
        self.assertEqual(stats["pushed"], stats["applied"])
        self.assertGreater(stats["splits"], 0)

    def test_tables_that_span_many_pages(self):
        # 101 nodes span four node pages, and 1,400 records run past a
        # 600-record queue's end and across its two record pages twice over.
        g = ag.Graph(frames=4, channel_count=2, buffers=100, nodes=101, queue=600)
        ids = [g.buffer() for _ in range(100)]
        nodes = [g.add(ag.CONST, ag.ALWAYS, (), b) for b in ids]
        g.add(ag.OUT, ag.ALWAYS, (ids[-2], ids[-1]), None)
        for round in range(1, 15):
            for i, n in enumerate(nodes):
                g.set(n, 0, float(round * 100 + i))
            self._pull(g)
        self.assertEqual([g.get(n, 0) for n in nodes], [1400.0 + i for i in range(100)])
        self.assertEqual(self._pull(g), [1498, 1499] * 4)
        self.assertEqual(g.stats()["overflows"], 0)

    def test_a_bad_set_is_refused_before_it_is_queued(self):
        g, a, _b = self._pair()
        with self.assertRaises((IndexError, ValueError)):
            g.set(99, 0, 1.0)
        self.assertEqual(g.stats()["pushed"], 0)
        with self.assertRaises(ValueError):
            ag.Graph(queue=-1)

    def test_a_carried_graph_renders_what_its_source_would_have(self):
        result = subprocess.run(
            [sys.executable,
             str(ROOT / "tests" / "parity" / "graph_carry_probe.py")],
            capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr)
        self.assertIn("GRAPH CARRY PASS", result.stdout)

    @staticmethod
    def _one(kind, arg=0.0, rate=48000):
        g = ag.Graph(sample_rate=rate, frames=8, buffers=2, nodes=2)
        return g, g.add(kind, ag.ALWAYS, (None,), g.buffer(), arg)

    def test_carry_refuses_another_kind(self):
        a, na = self._one(ag.OSC)
        b, nb = self._one(ag.LADDER)
        with self.assertRaisesRegex(ValueError, "kind"):
            a.carry(na, b, nb)

    def test_carry_refuses_another_arg(self):
        a, na = self._one(ag.FBDELAY, 20.0)
        b, nb = self._one(ag.FBDELAY, 30.0)
        with self.assertRaisesRegex(ValueError, "arg"):
            a.carry(na, b, nb)

    def test_carry_refuses_another_size(self):
        a, na = self._one(ag.FBDELAY, 20.0)
        b, nb = self._one(ag.FBDELAY, 20.0, rate=44100)
        with self.assertRaisesRegex(ValueError, "size"):
            a.carry(na, b, nb)

    def test_carry_refuses_another_rate(self):
        a, na = self._one(ag.OSC)
        b, nb = self._one(ag.OSC, rate=44100)
        with self.assertRaisesRegex(ValueError, "rate"):
            a.carry(na, b, nb)

    def test_carry_refuses_a_bad_node_or_a_non_graph(self):
        a, na = self._one(ag.OSC)
        b, nb = self._one(ag.OSC)
        for bad in ((5, b, nb), (na, b, 5), (-1, b, nb)):
            with self.assertRaises(ValueError):
                a.carry(*bad)
        with self.assertRaises(TypeError):
            a.carry(na, object(), nb)
        a.carry(na, a, na)                        # onto itself: nothing moves

    def test_a_carried_wow_table_outlives_its_source(self):
        a, na = self._one(ag.FBDELAY, 20.0)
        b, nb = self._one(ag.FBDELAY, 20.0)
        wow = array("h", [0, 8000, 0, -8000])
        for g, n in ((a, na), (b, nb)):
            g.set(n, ag.DELAY_WOW_HZ, 5.0)
            g.set(n, ag.DELAY_WOW_DEPTH_MS, 2.0)
        b.table(nb, wow)
        b.run(None)
        a.carry(na, b, nb)
        del b
        with self.assertRaises(BufferError):      # a holds it now
            wow.append(0)
        a.run(None)

    @staticmethod
    def _voice(frames=8):
        g = ag.Graph(frames=frames, channel_count=2, buffers=2, nodes=3)
        out = g.buffer()
        osc = g.add(ag.OSC, ag.ALWAYS, (None, None, None, None), out)
        g.set(osc, ag.OSC_FREQUENCY, 330.0)
        g.set(osc, ag.OSC_WAVEFORM, 1)
        g.set(osc, ag.OSC_LEVEL, 0.5)
        g.add(ag.OUT, ag.ALWAYS, (out, out), None)
        return g, osc

    def test_swap_refuses_a_non_graph_or_another_shape(self):
        a, _ = self._voice()
        with self.assertRaises(TypeError):
            a.swap(object())
        for kw in ({"frames": 16}, {"channel_count": 1}, {"sample_rate": 44100}):
            other = ag.Graph(**dict({"frames": 8, "channel_count": 2}, **kw))
            with self.assertRaisesRegex(ValueError, "differ"):
                a.swap(other)
        a.swap(a)                                 # onto itself: nothing moves

    def test_a_swap_mid_stream_plays_on_unbroken(self):
        ref, _ = self._voice()
        whole = [self._pull(ref) for _ in range(20)]
        a, osc_a = self._voice()
        heard = [self._pull(a) for _ in range(9)]
        b, osc_b = self._voice()
        b.carry(osc_b, a, osc_a)
        a.swap(b)                                 # a plays b's patch now
        heard += [self._pull(a) for _ in range(11)]
        self.assertEqual(heard, whole)
        self.assertEqual(a.sounding()[1], 20 * 8)  # the stream's frames stay with a
        self.assertEqual(b.sounding()[1], 0)       # and b keeps its own, none

    def test_swap_trades_the_patches(self):
        g, x, y = self._pair()
        g.set(x, 0, 100.0)
        g.set(y, 0, -100.0)
        self.assertEqual(self._pull(g), [100, -100] * 4)
        h, p, _q = self._pair()
        g.swap(h)
        self.assertEqual(self._pull(g), [0, 0] * 4)   # h's patch, never set
        self.assertEqual(self._pull(h), [100, -100] * 4)
        g.set(p, 0, 7.0)                          # a set on g reaches the patch it plays
        self.assertEqual(self._pull(g), [7, 0] * 4)

    def test_carrying_many_nodes_holds_a_table_once(self):
        wow = array("h", [0, 8000, 0, -8000])

        def delays():
            g = ag.Graph(frames=8, buffers=2, nodes=8)
            src, dst = g.buffer(), g.buffer()
            nodes = [g.add(ag.FBDELAY, ag.ALWAYS, (src,), dst, 20.0) for _ in range(8)]
            return g, nodes

        old, old_nodes = delays()
        old.table(old_nodes[0], wow)
        tracemalloc.start()
        for _ in range(6):                        # six patch changes in a row
            new, new_nodes = delays()
            for n, o in zip(new_nodes, old_nodes):
                new.carry(n, old, o)
            old, old_nodes = new, new_nodes
        grown = tracemalloc.get_traced_memory()[0]
        tracemalloc.stop()
        self.assertLess(grown, 64 * 1024)         # not a list that multiplies each time

    def test_a_deinited_graph_refuses_everything(self):
        g, a, _b = self._pair()
        g.deinit()
        with self.assertRaises(ValueError):
            g.set(a, 0, 1.0)
        g.deinit()                                # twice is fine


if __name__ == "__main__":
    unittest.main()
