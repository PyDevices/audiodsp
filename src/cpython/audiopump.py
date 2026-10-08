"""The audio pump, for CPython.

The same module the MicroPython and CircuitPython builds carry, with the same
names, the same status block and the same answers: `spawn()` adopts a graph,
`service()` pulls it a block at a time into a RAM ring, `drain()` takes the
bytes back out, and `Events`, `Tap` and `Ring` work as they do on a board.

What CPython does not get is a thread. This is the shape a build with no
platform driver has everywhere -- the WebAssembly port runs exactly this way --
so `driver()` is ``"none"``, `threaded()` is False, `spawn()` returns ``-2``,
and nothing is pulled until somebody calls `service()`. `audiodev`'s
`ServiceDriver` does that from its tick, and an app that drives the pump
itself does what a browser page does::

    status = bytearray(audiopump.STATUS_BYTES)
    ring = bytearray(16384)
    audiopump.spawn(graph, 0x7FFFFFFF, status, ring=ring)
    while playing:
        audiopump.service()
        n = audiopump.drain(buf)
        sink.write(memoryview(buf)[:n])

The graph is pulled through the same `_get_buffer` every node here
implements, so the bytes out of the ring are the bytes the native pump
produces from the same graph. The status block's FNV-1a digest is computed
the same way over the same bytes, which is what makes that checkable.

A pull never raises out of `service()` or `pull()`. A node that fails stops
the loop, and the status block's error and fault words say why, as they do on
a board -- see `fault()`.
"""

import struct as _struct
import sys as _sys
import time as _time

import _audiodsp
from audiocore import (
    DEINITED_MESSAGE as _DEINITED_MESSAGE, GET_BUFFER_DONE,
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, _borrow,
)

_fnv = _audiodsp.fnv1a64

STATUS_WORDS = 34
STATUS_BYTES = STATUS_WORDS * 8

# Why a service() call gave the thread back, packed as `blocks << 2 | why`.
SERVICE_SHIFT = 2
SERVICE_MASK = 3
SERVICE_MORE = 0
SERVICE_FULL = 1
SERVICE_PARKED = 2
SERVICE_DONE = 3

PRESS = 1
RELEASE = 2
RELEASE_ALL = 3
PLAY = 4
STOP = 5
LEVEL = 6
STRIKE = 7
CHOKE = 8

# The status block, word by word. The same layout as the native module's.
_BLOCKS, _BYTES, _DIGEST, _LAST_RESULT, _RUNNING, _ERROR, _TID, _PARKED = range(8)
_RING_W, _RING_R, _RING_OVF, _DRAIN_DIGEST = 8, 9, 10, 11
_PULL_US, _SINK_US, _WALL_US, _PARKS, _PARK_US = 12, 13, 16, 17, 18
_MAX_PULL_US = 20
_FAULT = 24
_FRAMES = 30
_EVENT_US_MAX = 31

# What `fault()` reports, and what the status block's fault word holds.
_FAULT_NONE = 0
_FAULT_NO_PROTOCOL = 1
_FAULT_DEINITED = 2
_FAULT_UNPUMPABLE = 3
_FAULT_IO = 4
_FAULT_LOOP = 5

_FNV_OFFSET = 0xCBF29CE484222325
_MASK64 = 0xFFFFFFFFFFFFFFFF
_MASK32 = 0xFFFFFFFF

#: Sources that read a file inside their pull. Refused at the door, wherever
#: they sit in the graph, as on every other build.
_UNPUMPABLE = ("WaveFile", "MP3Decoder")


def _now_us():
    return _time.perf_counter_ns() // 1000


def _has_protocol(sample):
    return (hasattr(sample, "_get_buffer")
            and hasattr(sample, "_reset_buffer"))


def _is_deinited(sample):
    namespace = getattr(sample, "__dict__", None)
    if namespace is not None:
        return bool(namespace.get("_deinited", False))
    try:
        # The native RawSample keeps no __dict__; its fields raise instead.
        sample.channel_count
    except ValueError:
        return True
    return False


def _check(sample):
    """What the native spawn() checks before it adopts a graph."""
    if not _has_protocol(sample):
        raise TypeError("object does not implement the audiocore sample "
                        "protocol")
    if _is_deinited(sample):
        raise ValueError(_DEINITED_MESSAGE)


def _max_block(sample):
    """Bytes one pull of `sample` can hand back, or 0 if it does not say.

    The native nodes all carry this in their base struct. Here a node that
    has `max_buffer_length` says so; the ported CircuitPython nodes and the
    Mixer have `buffer_size`, which is the same number on the native side;
    a Synthesizer renders 256 frames a pull.
    """
    length = getattr(sample, "max_buffer_length", 0) or 0
    if length:
        return int(length)
    length = getattr(sample, "buffer_size", 0) or 0
    if length:
        return int(length)
    if type(sample).__name__ == "Synthesizer":
        return 256 * 2 * int(sample.channel_count)
    return 0


def _find_unpumpable(sample):
    """The first file-backed source anywhere behind `sample`, or None.

    Walks what each node holds: its source, a mixer's voices, a splitter's
    taps. A node that holds its source somewhere this cannot see is reported
    clean, which is the same direction the native walk errs in.
    """
    seen = set()
    stack = [sample]
    while stack:
        node = stack.pop()
        if node is None or id(node) in seen:
            continue
        seen.add(id(node))
        if type(node).__name__ in _UNPUMPABLE:
            return node
        namespace = getattr(node, "__dict__", None)
        if not namespace:
            continue
        for value in namespace.values():
            if isinstance(value, (tuple, list)):
                stack.extend(item for item in value
                             if _has_protocol(item)
                             or type(item).__name__ == "MixerVoice")
            elif _has_protocol(value) or type(value).__name__ == "MixerVoice":
                stack.append(value)
    return None


def _refuse_unpumpable(sample):
    found = _find_unpumpable(sample)
    if found is not None:
        raise ValueError(
            "a file-backed source cannot be pumped; it reads through the VFS "
            "inside the pull. Fill a ring from the interpreter instead. "
            "(found a %s in the graph)" % type(found).__name__)


def _fault_of(exc):
    """The fault code a native node would have left for this exception."""
    if isinstance(exc, RecursionError):
        return _FAULT_LOOP
    if isinstance(exc, ValueError) and str(exc) == _DEINITED_MESSAGE:
        return _FAULT_DEINITED
    if isinstance(exc, TypeError) and "sample protocol" in str(exc):
        return _FAULT_NO_PROTOCOL
    if isinstance(exc, OSError):
        return _FAULT_IO
    return None


class _Context:
    """Everything the loop carries between service() calls."""

    def __init__(self):
        self.sample = None
        self.status = None
        self.blocks = 0
        self.ring = None
        self.ring_len = 0
        self.ring_w = 0
        self.ring_r = 0
        self.ring_wpos = 0
        self.ring_rpos = 0
        self.ring_w_total = 0
        self.ring_r_total = 0
        self.drain_digest = 0
        self.pending = None
        self.frames = 0
        self.frame_bytes = 0
        self.block_frames = 1
        self.max_block = 0
        self.stop = False
        self.park_req = False
        self.parked = False
        self.finished = False
        self.loop = False
        self.retarget_req = False
        self.retarget_loop = -1
        self.service_mode = False
        self.begun = False
        self.digest = _FNV_OFFSET
        self.done = 0
        self.bytes = 0
        self.error = 0
        self.result = GET_BUFFER_MORE_DATA
        self.pull_us = 0
        self.max_pull_us = 0
        self.park_us = 0
        self.parks = 0
        self.ring_ovf = 0
        self.event_us_max = 0
        self.wall_start = 0
        self.park_enter_us = 0

    def put(self, index, value):
        _struct.pack_into("<Q", self.status, index * 8, value & _MASK64)


_ctx = _Context()
_live = False
_events = None
_tap = None
_fault = _FAULT_NONE


def _prepare(sample, blocks, status, ring):
    global _ctx, _fault
    view = memoryview(status)
    if view.readonly or view.nbytes < STATUS_BYTES:
        raise ValueError("status too small")
    if ring is not None:
        ring_view = memoryview(ring).cast("B")
        if ring_view.readonly:
            raise TypeError("ring must be writable")
        if ring_view.nbytes < 64:
            raise ValueError("ring too small")
    _check(sample)
    view.cast("B")[:STATUS_BYTES] = bytes(STATUS_BYTES)
    _fault = _FAULT_NONE
    ctx = _Context()
    ctx.status = status
    if ring is not None:
        ctx.ring = ring_view
        ctx.ring_len = ring_view.nbytes
    ctx.sample = sample
    ctx.max_block = _max_block(sample)
    ctx.frame_bytes = (int(sample.channel_count)
                       * (int(sample.bits_per_sample) // 8))
    if ctx.frame_bytes and ctx.max_block:
        ctx.block_frames = max(1, ctx.max_block // ctx.frame_bytes)
    ctx.blocks = int(blocks)
    _ctx = ctx
    return ctx


def _open_sink(sink):
    # A file or an I2S channel is the platform driver's to open, and this
    # build has none -- the same refusal a native build with no driver gives.
    if sink is None or sink is False:
        return
    if isinstance(sink, (str, bytes)):
        raise ValueError("this build's driver has no file sink")
    if sink:
        raise ValueError("no i2s sink open")


def _run_begin(ctx):
    ctx.digest = _FNV_OFFSET
    ctx.result = GET_BUFFER_MORE_DATA
    ctx.put(_RUNNING, 1)
    ctx.put(_TID, 0)
    ctx.wall_start = _now_us()
    ctx.begun = True


def _run_end(ctx):
    ctx.put(_WALL_US, _now_us() - ctx.wall_start)
    ctx.put(_DIGEST, ctx.digest)
    ctx.put(_LAST_RESULT, ctx.result)
    ctx.put(_ERROR, ctx.error)
    ctx.put(_RUNNING, 0)
    ctx.finished = True


def _ring_write(ctx, data):
    length = len(data)
    at = ctx.ring_wpos
    first = min(ctx.ring_len - at, length)
    ring = ctx.ring
    ring[at:at + first] = data[:first]
    if length > first:
        ring[:length - first] = data[first:]
    ctx.ring_wpos = (at + length) % ctx.ring_len
    ctx.ring_w_total += length
    ctx.ring_w = (ctx.ring_w + length) & _MASK32
    ctx.put(_RING_W, ctx.ring_w_total)


def _room(ctx):
    return ctx.ring_len - ((ctx.ring_w - ctx.ring_r) & _MASK32)


def _pull_one(ctx):
    """One block from the tail: (result, data) or None after a failure."""
    global _fault
    sample = ctx.sample
    if _is_deinited(sample):
        _fault = _FAULT_DEINITED
        ctx.error = 1
        ctx.put(_FAULT, _fault)
        return None
    try:
        result, data = _borrow(sample, False, 0)
    except Exception as exc:  # noqa: BLE001 - a pull reports, never raises
        code = _fault_of(exc)
        if code is None:
            # Not one of the failures a native node can report. Say what it
            # was, the way an unraisable exception is said, and stop.
            import traceback
            print("audiopump: the pull raised; the pump has stopped",
                  file=_sys.stderr)
            traceback.print_exception(type(exc), exc, exc.__traceback__,
                                      file=_sys.stderr)
            ctx.error = 1
        else:
            _fault = code
            ctx.error = 5
            ctx.put(_FAULT, code)
        return None
    if result == GET_BUFFER_ERROR:
        ctx.error = 1
        ctx.put(_FAULT, _fault)
        return None
    return result, data


def _run_blocks(ctx, budget):
    global _fault
    why = SERVICE_DONE
    spent = 0
    while ctx.done < ctx.blocks and not ctx.stop:
        if spent >= budget:
            why = SERVICE_MORE
            break
        if ctx.service_mode and ctx.ring is not None:
            # A block that did not fit last time goes first. It is held rather
            # than dropped, which only a tail that misstated its block size
            # ever needs.
            if ctx.pending is not None:
                if _room(ctx) < len(ctx.pending):
                    why = SERVICE_FULL
                    break
                _ring_write(ctx, ctx.pending)
                ctx.pending = None
            need = ctx.max_block or 1
            if need <= ctx.ring_len and _room(ctx) < need:
                why = SERVICE_FULL
                break
        if ctx.park_req and ctx.service_mode:
            if not ctx.parked:
                ctx.parks += 1
                ctx.parked = True
                ctx.put(_PARKED, 1)
                ctx.put(_PARKS, ctx.parks)
                ctx.park_enter_us = _now_us()
            why = SERVICE_PARKED
            break
        if ctx.service_mode and ctx.parked:
            ctx.parked = False
            ctx.put(_PARKED, 0)
            ctx.park_us += _now_us() - ctx.park_enter_us
            ctx.put(_PARK_US, ctx.park_us)

        t0 = _now_us()
        if ctx.retarget_req:
            ctx.retarget_req = False
            if ctx.retarget_loop >= 0:
                ctx.loop = ctx.retarget_loop != 0
                ctx.retarget_loop = -1
            ctx.max_block = _max_block(ctx.sample)
        if _events is not None:
            e0 = _now_us()
            _events._apply(ctx.frames, ctx.block_frames)
            edt = _now_us() - e0
            if edt > ctx.event_us_max:
                ctx.event_us_max = edt
                ctx.put(_EVENT_US_MAX, edt)
        got = _pull_one(ctx)
        dt = _now_us() - t0
        ctx.pull_us += dt
        if dt > ctx.max_pull_us:
            ctx.max_pull_us = dt
        if got is None:
            break
        result, data = got
        ctx.result = result
        if ctx.stop:
            break
        length = len(data)
        ctx.digest = _fnv(data, ctx.digest)

        if ctx.ring is not None and length:
            if ((ctx.ring_w - ctx.ring_r) & _MASK32) + length <= ctx.ring_len:
                _ring_write(ctx, data)
            elif ctx.service_mode and length <= ctx.ring_len:
                ctx.pending = bytes(data)
            else:
                ctx.ring_ovf += 1
                ctx.put(_RING_OVF, ctx.ring_ovf)

        if _tap is not None and length:
            _tap._write(data)

        if ctx.frame_bytes:
            nframes = length // ctx.frame_bytes
            if nframes:
                ctx.block_frames = nframes
            ctx.frames = (ctx.frames + nframes) & _MASK32
            ctx.put(_FRAMES, ctx.frames)

        ctx.bytes += length
        ctx.done += 1
        spent += 1
        ctx.put(_BLOCKS, ctx.done)
        ctx.put(_BYTES, ctx.bytes)
        ctx.put(_PULL_US, ctx.pull_us)
        ctx.put(_MAX_PULL_US, ctx.max_pull_us)
        if result == GET_BUFFER_DONE:
            if not ctx.loop:
                ctx.error = 3
                break
            # The block that came with the DONE has been played; only now is
            # the tail rewound, as CircuitPython's own outputs do it.
            try:
                ctx.sample._reset_buffer(False, 0)
            except Exception as exc:  # noqa: BLE001 - reported, not raised
                _fault = _fault_of(exc) or _FAULT_NONE
                ctx.error = 1
                ctx.put(_FAULT, _fault)
                break
            ctx.result = GET_BUFFER_MORE_DATA
        if ctx.pending is not None:
            why = SERVICE_FULL
            break
    return why


# --- the module's surface ---------------------------------------------------


def pull(sample, blocks, status, sink=None, ring=None):
    """Pull `blocks` blocks of `sample` on this thread, start to finish."""
    ctx = _prepare(sample, blocks, status, ring)
    _open_sink(sink)
    _run_begin(ctx)
    _run_blocks(ctx, float("inf"))
    _run_end(ctx)


def spawn(sample, blocks, status, sink=None, ring=None, core=-1, prio=4,
          stack=16384, psram=False, timeout_ms=200, pace=False, loop=False):
    """Adopt `sample` as the pump's tail. Returns -2: there is no thread.

    Not one block is pulled here. `service()` runs the loop; `core`, `prio`,
    `stack`, `psram`, `timeout_ms` and `pace` are accepted for the boards'
    sake and mean nothing without a thread.
    """
    global _live
    if _live:
        raise ValueError(
            "a pump is already spawned; call audiopump.shutdown() first")
    _refuse_unpumpable(sample)
    ctx = _prepare(sample, blocks, status, ring)
    ctx.loop = bool(loop)
    ctx.retarget_loop = -1
    _open_sink(sink)
    ctx.service_mode = True
    _live = True
    _run_begin(ctx)
    return -2


def service(max_blocks=0):
    """Advance the pump. Returns ``blocks << SERVICE_SHIFT | why``.

    `max_blocks` caps the blocks this call pulls; 0 means as many as the
    output ring has room for. `why` is SERVICE_MORE (the cap was reached),
    SERVICE_FULL (the ring is full: drain it), SERVICE_PARKED or SERVICE_DONE
    (the loop has ended; the status block says why).
    """
    ctx = _ctx
    budget = int(max_blocks) or float("inf")
    before = ctx.done
    why = SERVICE_MORE
    if ctx.service_mode:
        if _live and not ctx.finished:
            why = _run_blocks(ctx, budget)
            if why == SERVICE_DONE:
                _run_end(ctx)
        else:
            why = SERVICE_DONE
            before = ctx.done
    return ((ctx.done - before) << SERVICE_SHIFT) | why


def drain(out):
    """Move what the pump has made into `out`. Returns the bytes moved."""
    view = memoryview(out).cast("B")
    if view.readonly:
        raise TypeError("drain() wants a writable buffer")
    ctx = _ctx
    if ctx.ring is None:
        return 0
    take = min((ctx.ring_w - ctx.ring_r) & _MASK32, view.nbytes)
    if take == 0:
        return 0
    at = ctx.ring_rpos
    first = min(ctx.ring_len - at, take)
    ring = ctx.ring
    view[:first] = ring[at:at + first]
    if take > first:
        view[first:take] = ring[:take - first]
    ctx.drain_digest = _fnv(view[:take], ctx.drain_digest or _FNV_OFFSET)
    ctx.ring_rpos = (at + take) % ctx.ring_len
    ctx.ring_r_total += take
    ctx.ring_r = (ctx.ring_r + take) & _MASK32
    ctx.put(_RING_R, ctx.ring_r_total)
    ctx.put(_DRAIN_DIGEST, ctx.drain_digest)
    return take


def join(timeout_ms=30000):
    """True once the loop has ended and the pump has let go of the graph.

    There is no thread to wait for, so this never pulls the rest of the graph
    itself: a pump that has not finished says False, and the caller keeps
    calling `service()`.
    """
    global _live
    if not _live:
        return True
    if not _ctx.finished:
        return False
    _live = False
    return True


def stop():
    """Ask the loop to end. The next `service()` ends it."""
    _ctx.stop = True
    _ctx.park_req = False


def park(timeout_us=100000):
    """Hold the pump at a block boundary. Always at one here: True."""
    if not _live or _ctx.finished:
        return True
    _ctx.park_req = True
    return True


def unpark():
    _ctx.park_req = False


def retarget(sample, *, loop=None):
    """Point the pump at a different tail, from the next block on.

    `loop` None leaves the loop flag as it is; True or False sets it for the
    new tail.
    """
    _refuse_unpumpable(sample)
    _check(sample)
    ctx = _ctx
    ctx.sample = sample
    ctx.retarget_loop = -1 if loop is None else (1 if loop else 0)
    ctx.retarget_req = True


def running():
    """True while a pump is adopted and its loop has not finished."""
    return _live and not _ctx.finished


def shutdown():
    """Let go of everything: the graph, the ring, the queue and the tap."""
    global _ctx, _live, _events, _tap
    _ctx.stop = True
    _ctx.park_req = False
    _live = False
    _ctx = _Context()
    _events = None
    _tap = None


def reset(sample):
    """Rewind `sample`, as the pump does when a looped tail ends."""
    if not _has_protocol(sample):
        raise TypeError("object does not implement the audiocore sample "
                        "protocol")
    sample._reset_buffer(False, 0)


def info(sample):
    """(sample_rate, channel_count, bits_per_sample, max_buffer_length,
    samples_signed, single_buffer) -- what the graph says it is."""
    _check(sample)
    return (int(sample.sample_rate), int(sample.channel_count),
            int(sample.bits_per_sample), _max_block(sample),
            bool(sample.samples_signed),
            bool(getattr(sample, "single_buffer", False)))


def now():
    """Frames the pump has pulled since spawn(). Wraps at 2**32."""
    return _ctx.frames


def threaded():
    """False: the loop runs inside `service()`, on the caller's thread."""
    return False


def backpressure():
    """True: a full ring makes `service()` return rather than drop a block."""
    return True


def driver():
    """Which platform driver is bound: ``"none"`` on CPython."""
    return "none"


def events(*args):
    """Attach an `Events` queue, or detach it with None. Returns the queue
    that is attached; called with no argument it only reports."""
    global _events
    if args:
        queue = args[0]
        if queue is not None and not isinstance(queue, Events):
            raise TypeError("expected an Events queue")
        _events = queue
    return _events


def tap(*args):
    """Attach a `Tap`, or detach it with None. Returns the tap attached."""
    global _tap
    if args:
        probe = args[0]
        if probe is not None and not isinstance(probe, Tap):
            raise TypeError("expected a Tap")
        _tap = probe
    return _tap


def lock_stats():
    """The pump lock's counters. Nothing contends for it without a thread."""
    return (0, 0, 0, 0, 0, 0, 0)


def lock_reset():
    """Zero the lock's counters and clear the last fault."""
    global _fault
    _fault = _FAULT_NONE


def fault():
    """Why the last pull gave up, as the native fault codes: 0 none, 1 not an
    audio node, 2 released while playing, 3 a file-backed source, 4 a read
    that raised, 5 a graph that leads back into itself."""
    return _fault


# --- the queue --------------------------------------------------------------


def _before(a, b):
    """`a` is earlier than `b`, across the 2**32 wrap."""
    return ((a - b) & _MASK32) >= 0x80000000


class Events:
    """Frame-stamped events the pump applies at the top of a block.

    ``queue.at(frame, op, target, arg=None, loop=False)`` schedules one and
    returns a token for `cancel()`. Everything that could refuse is checked
    here, when it is scheduled; applying one never raises.
    """

    def __init__(self, *, capacity=64):
        capacity = int(capacity)
        if capacity < 1 or capacity > 4096:
            raise ValueError("capacity is 1..4096")
        self._capacity = capacity
        self._ev = []           # [frame, token, op, target, arg, loop], sorted
        self._next_token = 0
        self._scheduled = 0
        self._applied = 0
        self._late = 0
        self._cancelled = 0
        self._dropped = 0
        self._refused = 0

    def _validate(self, op, target, arg):
        import audiomixer
        import synthio

        if op in (PRESS, RELEASE, RELEASE_ALL):
            if not isinstance(target, synthio.Synthesizer):
                raise TypeError("press/release want a Synthesizer")
            if _is_deinited(target):
                raise ValueError(_DEINITED_MESSAGE)
            if op == RELEASE_ALL:
                return None
            if isinstance(arg, int) and not isinstance(arg, bool):
                if arg < 0 or arg > 127:
                    raise ValueError("a MIDI note is 0..127")
            elif not isinstance(arg, synthio.Note):
                raise TypeError(
                    "a scheduled note is a Note or a MIDI number -- an "
                    "iterable of them is several events")
            return arg
        if op in (PLAY, STOP, LEVEL):
            if not isinstance(target, audiomixer.MixerVoice):
                raise TypeError("play/stop/level want a MixerVoice")
            if getattr(target, "_mixer", None) is None:
                raise ValueError("the voice is not in a Mixer")
            if op == PLAY:
                target._mixer._check_sample(arg)
                return arg
            if op == STOP:
                return None
            return arg
        if op in (STRIKE, CHOKE):
            import audiomodal

            if not isinstance(target, audiomodal.Bank):
                raise TypeError("strike/choke want an audiomodal.Bank")
            if _is_deinited(target):
                raise ValueError(_DEINITED_MESSAGE)
            if op == CHOKE:
                return None
            try:
                table = memoryview(arg)
            except TypeError:
                table = None
            if table is None or table.format != "f":
                raise TypeError("a strike's table is an array('f') of "
                                "frequency, decay, gain per mode")
            floats = table.nbytes // 4
            if floats == 0 or floats % 3:
                raise ValueError("a strike's table needs three floats a mode")
            if floats // 3 > target.modes:
                raise ValueError("table has %d modes, bank holds %d"
                                 % (floats // 3, target.modes))
            return arg
        raise ValueError("unknown op")

    def at(self, frame, op, target, arg=None, loop=False):
        """Schedule `op` on `target` at `frame`. Returns its token, or 0 when
        the queue was full."""
        frame = int(frame) & _MASK32
        op = int(op)
        arg = self._validate(op, target, arg)
        if len(self._ev) >= self._capacity:
            self._dropped += 1
            return 0
        self._next_token = (self._next_token + 1) & _MASK32
        if self._next_token == 0:
            self._next_token = 1    # 0 is "refused", never a live token
        token = self._next_token
        i = len(self._ev)
        while i > 0 and _before(frame, self._ev[i - 1][0]):
            i -= 1
        self._ev.insert(i, [frame, token, op, target, arg, bool(loop)])
        self._scheduled += 1
        return token

    def cancel(self, token):
        token = int(token) & _MASK32
        for i, event in enumerate(self._ev):
            if event[1] == token:
                del self._ev[i]
                self._cancelled += 1
                return True
        return False

    def clear(self):
        was = len(self._ev)
        self._ev = []
        self._cancelled += was
        return was

    def stats(self):
        """(scheduled, applied, late, cancelled, dropped, refused, pending,
        capacity)."""
        return (self._scheduled, self._applied, self._late, self._cancelled,
                self._dropped, self._refused, len(self._ev), self._capacity)

    def pending(self):
        return len(self._ev)

    def _apply(self, now, block_frames):
        ev = self._ev
        if not ev:
            return 0
        end = (now + block_frames) & _MASK32
        n = 0
        while n < len(ev) and _before(ev[n][0], end):
            frame, _token, op, target, arg, loop = ev[n]
            if _before(frame, now):
                self._late += 1
            self._apply_one(op, target, arg, loop)
            n += 1
        if n:
            self._applied += n
            del ev[:n]
        return n

    def _apply_one(self, op, target, arg, loop):
        try:
            if op in (PRESS, RELEASE, RELEASE_ALL):
                if _is_deinited(target):
                    self._refused += 1
                elif op == PRESS:
                    target.press(arg)
                elif op == RELEASE:
                    target.release(arg)
                else:
                    target.release_all()
            elif op == PLAY:
                if _is_deinited(arg) or _is_deinited(target._mixer):
                    self._refused += 1
                else:
                    try:
                        target.play(arg, loop=loop)
                    except ValueError:
                        # A looped sample too short to fill one word.
                        self._refused += 1
            elif op == STOP:
                target.stop()
            elif op == LEVEL:
                target.level = arg
            elif op == STRIKE:
                if _is_deinited(target):
                    self._refused += 1
                else:
                    rows = memoryview(arg).cast("B").cast("f")
                    target.set_modes([tuple(rows[i:i + 3])
                                      for i in range(0, len(rows), 3)])
            elif op == CHOKE:
                if _is_deinited(target):
                    self._refused += 1
                else:
                    target.clear()
        except Exception:  # noqa: BLE001 - applying an event never raises
            self._refused += 1


# --- the tap ----------------------------------------------------------------


class Tap:
    """A window onto what the pump played, for a meter, a scope or a recorder.

    The pump copies every block into it after the tail; `readinto(buf)` hands
    back the newest whole frames that fit, and 0 when nothing has been played
    since the last read. A reader that falls behind loses old audio, never the
    pump's time.
    """

    def __init__(self, *, frames=2048, channel_count=2):
        channels = int(channel_count)
        frames = int(frames)
        if channels < 1 or channels > 2 or frames < 16:
            raise ValueError("bad channel_count or frames")
        frame = channels * 2
        cap = 1
        while cap < frames * frame:
            cap <<= 1
        if cap > (1 << 22):
            raise ValueError("tap too large")
        self._buf = bytearray(cap)
        self._cap = cap
        self._frame = frame
        self._w = 0
        self._blocks = 0
        self._reads = 0
        self._torn = 0
        self._last_read = 0

    def _write(self, data):
        length = len(data)
        if length > self._cap:
            data = data[length - self._cap:]
            length = self._cap
        at = self._w & (self._cap - 1)
        first = min(self._cap - at, length)
        self._buf[at:at + first] = data[:first]
        if length > first:
            self._buf[:length - first] = data[first:]
        self._blocks += 1
        self._w = (self._w + length) & _MASK32

    def readinto(self, buf):
        out = memoryview(buf).cast("B")
        want = min(out.nbytes, self._cap)
        want -= want % self._frame
        if want == 0:
            return 0
        w = self._w
        if w == self._last_read:
            return 0
        if w < want:
            want = w - (w % self._frame)
            if want == 0:
                return 0
        start = (w - want) & _MASK32
        at = start & (self._cap - 1)
        first = min(self._cap - at, want)
        out[:first] = self._buf[at:at + first]
        if want > first:
            out[first:want] = self._buf[:want - first]
        self._reads += 1
        self._last_read = w
        return want

    def stats(self):
        """(bytes written, blocks, reads, torn, capacity, frame bytes)."""
        return (self._w, self._blocks, self._reads, self._torn, self._cap,
                self._frame)


# --- the push side ----------------------------------------------------------


class Ring(_AudioSample):
    """A stream Python pushes into and the pump pulls as an ordinary source.

    Signed 16-bit. `write()` takes whole frames and returns the bytes it
    accepted; `space()` is bytes. A pull that finds less than a block hands
    out a block of silence and keeps what it has, and counts an underrun.
    """

    def __init__(self, *, sample_rate=48000, channel_count=2, frames=256,
                 capacity=4):
        channels = int(channel_count)
        frames = int(frames)
        capacity = int(capacity)
        if channels < 1 or channels > 2 or frames < 1 or capacity < 2:
            raise ValueError("bad channel_count, frames or capacity")
        if frames * channels * 2 * capacity > (1 << 24):
            raise ValueError("ring too large")
        self._frame = channels * 2
        self._block = frames * self._frame
        self._cap = self._block * capacity
        self._ring = bytearray(self._cap)
        self._out = (bytearray(self._block), bytearray(self._block))
        self._which = 0
        self._w = 0
        self._r = 0
        self._wpos = 0
        self._rpos = 0
        self._underruns = 0
        self._overruns = 0
        self._pulls = 0
        self._wrote = 0
        self._read = 0
        self._starved = 0
        self._in_digest = _FNV_OFFSET
        self._out_digest = _FNV_OFFSET
        self.sample_rate = int(sample_rate)
        self.channel_count = channels
        self.bits_per_sample = 16
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = self._block
        self._deinited = False

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._which = 0

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        dst = self._out[self._which]
        self._which ^= 1
        block = self._block
        if self._w - self._r < block:
            dst[:] = bytes(block)
            self._underruns += 1
            self._starved += block
        else:
            at = self._rpos
            first = min(self._cap - at, block)
            dst[:first] = self._ring[at:at + first]
            if block > first:
                dst[first:] = self._ring[:block - first]
            self._out_digest = _fnv(dst, self._out_digest)
            self._read += block
            self._rpos = (at + block) % self._cap
            self._r += block
        self._pulls += 1
        return GET_BUFFER_MORE_DATA, memoryview(dst)

    def write(self, buf):
        self._check()
        data = memoryview(buf).cast("B")
        room = self._cap - (self._w - self._r)
        n = data.nbytes
        if n > room:
            n = room
            self._overruns += 1
        n -= n % self._frame
        if n == 0:
            return 0
        at = self._wpos
        first = min(self._cap - at, n)
        self._ring[at:at + first] = data[:first]
        if n > first:
            self._ring[:n - first] = data[first:n]
        self._in_digest = _fnv(data[:n], self._in_digest)
        self._wrote += n
        self._wpos = (at + n) % self._cap
        self._w += n
        return n

    def space(self):
        self._check()
        room = self._cap - (self._w - self._r)
        return room - (room % self._frame)

    def level(self):
        self._check()
        return self._w - self._r

    def stats(self):
        """(wrote, read, starved, underruns, overruns, pulls, in_digest,
        out_digest, capacity, block)."""
        return (self._wrote, self._read, self._starved, self._underruns,
                self._overruns, self._pulls, self._in_digest,
                self._out_digest, self._cap, self._block)

    def clear(self):
        self._check()
        self._r = self._w
        self._rpos = self._wpos


__all__ = (
    "Events", "Ring", "Tap", "backpressure", "drain", "driver", "events",
    "fault", "info", "join", "lock_reset", "lock_stats", "now", "park",
    "pull", "reset", "retarget", "running", "service", "shutdown", "spawn",
    "stop", "tap", "threaded", "unpark",
)
