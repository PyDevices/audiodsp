# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Run an audio graph off the interpreter thread.

`spawn` pulls a graph of audio samples (a `audiomixer.Mixer`, a
`synthio.Synthesizer`, effects) on a thread the interpreter is not on, so the
audio keeps an exact clock while a screen redraws or Python does anything
else. A platform driver supplies the thread and the output; `driver` says
which one bound. Where there is no thread, the pump runs in service mode:
`spawn` adopts the graph and nothing is pulled until you call `service`.

`Ring` lets Python push PCM into the graph, `DriftResampler` plays a ring that
something else fills on its own clock, `Events` schedules notes on the audio
clock, and `Tap` reads what is going out.

The CPython wheel and every MicroPython port carry this whole module.
CircuitPython carries only `Ring` and `DriftResampler`, because its playback
layer already pulls from its own audio thread.
"""

from typing import Protocol, Self

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

STATUS_WORDS: int
"""How many 64-bit words the status block holds: 34."""
STATUS_BYTES: int
"""How many bytes the status block holds, which is the size of the buffer
`spawn` and `pull` take for ``status``: 272."""

SERVICE_MORE: int
"""`service` result: there is more to do."""
SERVICE_FULL: int
"""`service` result: the output ring has no room for another block."""
SERVICE_PARKED: int
"""`service` result: the pump is parked."""
SERVICE_DONE: int
"""`service` result: the pump has ended."""
SERVICE_MASK: int
"""Mask for the reason in a `service` result: ``result & SERVICE_MASK``."""
SERVICE_SHIFT: int
"""Shift for the block count in a `service` result:
``result >> SERVICE_SHIFT``."""

PRESS: int
"""`Events.at` operation: press a note on a `synthio.Synthesizer`."""
RELEASE: int
"""`Events.at` operation: release a note on a `synthio.Synthesizer`."""
RELEASE_ALL: int
"""`Events.at` operation: release every note on a `synthio.Synthesizer`."""
PLAY: int
"""`Events.at` operation: play a sample on an `audiomixer.MixerVoice`."""
STOP: int
"""`Events.at` operation: stop an `audiomixer.MixerVoice`."""
LEVEL: int
"""`Events.at` operation: set an `audiomixer.MixerVoice`'s level."""
STRIKE: int
"""`Events.at` operation: strike an `audiomodal.Bank` with a table of modes."""
CHOKE: int
"""`Events.at` operation: stop an `audiomodal.Bank` ringing."""

def spawn(
    sample: _AudioSample,
    blocks: int,
    status: bytearray | memoryview,
    sink: str | bool | None = None,
    ring: bytearray | memoryview | None = None,
    core: int = -1,
    prio: int = 21,
    stack: int = 16384,
    psram: bool = False,
    timeout_ms: int = 200,
    pace: bool = False,
    loop: bool = False,
) -> int:
    """Start pulling a graph on a thread of its own.

    Only one pump can run at a time; call `shutdown` before spawning another.
    A graph made of a `audiocore.WaveFile`, `audiomp3.MP3Decoder` or
    `audiovorbis.VorbisDecoder` is refused, because those read their files
    through the interpreter inside the pull: fill a `Ring` from the
    interpreter instead.

    Args:
        sample: The tail of the graph to pull.
        blocks: How many blocks to pull before stopping. A large number such
            as 0x7FFFFFFF plays until `stop` or `shutdown`.
        status: A writable buffer of at least `STATUS_BYTES` bytes that the
            pump fills with counters, as `STATUS_WORDS` little-endian 64-bit
            words. Word 0 is blocks pulled, 1 bytes seen, 2 an FNV-1a
            digest of every byte, 3 the last pull's result, 4 whether the
            loop is running, 5 the error (0 none, 1 buffer, 2 null, 3 done),
            14 sink timeouts and 24 the fault code. Read it with
            ``struct.unpack``.
        sink: Where the audio goes. True sends it to the board's I2S output
            when the driver has opened one; a path writes it to a file on a
            desktop. None sends it nowhere.
        ring: A writable buffer of at least 64 bytes to write the audio into
            instead, which `drain` empties.
        core: The CPU core to run on, on ports that have cores. -1 lets the
            driver choose.
        prio: The thread's priority, on ports that have one.
        stack: The thread's stack in bytes, on ports that need one.
        psram: Whether the thread's stack goes in PSRAM, where the port has
            it.
        timeout_ms: How long the pump waits on a full sink before counting a
            timeout.
        pace: On a desktop, pull at the graph's own sample rate in real time.
        loop: Rewind the tail and carry on when it reports it is done, as
            ``audiobusio.I2SOut.play(sample, loop=True)`` does.

    Returns:
        The core the thread runs on, or -1 when it has no core affinity, or -2
        in service mode, where no thread was made.

    Raises:
        ValueError: A pump is already spawned, the status or ring buffer is
            too small, the graph contains a file-backed source, or
            ``sink=True`` when the driver has no output open.
        OSError: The thread could not be made, or a file ``sink`` could not
            be opened.
    """

def pull(
    sample: _AudioSample,
    blocks: int,
    status: bytearray | memoryview,
    sink: str | bool | None = None,
    ring: bytearray | memoryview | None = None,
    /,
) -> None:
    """Run the pump's loop on the calling thread until it ends.

    It is the same loop `spawn` runs, with the same arguments, so it can be
    measured on the interpreter thread. Wrap it in ``micropython.heap_lock()``
    to check that a pull does not allocate.
    """

def service(max_blocks: int = 0, /) -> int:
    """Advance the pump where there is no thread to do it.

    Args:
        max_blocks: The most blocks to produce. 0 means as many as the output
            ring has room for.

    Returns:
        ``blocks << SERVICE_SHIFT | why``, where ``why`` is one of
        `SERVICE_MORE`, `SERVICE_FULL`, `SERVICE_PARKED` and `SERVICE_DONE`.
        Unpack it with ``why = r & SERVICE_MASK`` and
        ``blocks = r >> SERVICE_SHIFT``. It is one small integer, so a call
        under ``micropython.heap_lock()`` does not allocate. On a threaded
        port it does nothing and returns `SERVICE_MORE` with no blocks.
    """

def drain(out: bytearray | memoryview, /) -> int:
    """Copy audio from the ring given to `spawn` into a buffer.

    Args:
        out: A writable buffer.

    Returns:
        How many bytes were copied. 0 when there is no ring or it is empty.
    """

def join(timeout_ms: int = 30000, /) -> bool:
    """Wait for the pump to end, and release its thread and sink.

    Returns:
        True when the pump had ended or was never running; False when it
        was still running at the deadline. In service mode it does not pull
        anything itself, so it reports False until `service` has finished
        the graph.
    """

def stop() -> None:
    """Ask the pump to stop after the block it is on. Follow with `join`."""

def shutdown() -> None:
    """Stop the pump, release the graph and close the sink. It is safe to
    call twice, and a soft reset does it for you."""

def running() -> bool:
    """Whether a pump is running."""

def park(timeout_us: int = 100000, *, fade: bool = False) -> bool:
    """Hold the pump at a block boundary so the graph can be rewired.

    The pump does not touch the graph again until `unpark`.

    Args:
        timeout_us: How long to wait for the pump to reach a boundary, in
            microseconds.
        fade: True makes the park a hold with a deadline: the block the pump
            holds goes out faded, and the pump feeds silence until `unpark`.

    Returns:
        True once the pump is parked, or has ended.
    """

def unpark() -> None:
    """Let a parked pump carry on."""

def retarget(
    sample: _AudioSample,
    *,
    loop: bool | None = None,
    fade: bool = False,
    timeout_us: int = 100000,
) -> bool | None:
    """Point the pump at a different tail while it runs.

    Everything that can raise happens first, on the calling thread; then the
    pump either sees the whole new tail or the whole old one.

    Args:
        sample: The new tail.
        loop: Whether the new tail loops. It takes effect with the swap, so
            it is right for the tail that plays. None leaves the flag as
            `spawn` set it.
        fade: True fades the old tail's next block to silence, swaps, and
            fades the new tail's first block in, a dip of one block each way,
            instead of a cut that clicks.
        timeout_us: With ``fade``, how long to wait for the swap.

    Returns:
        With ``fade``, True once the pump has stopped pulling the old tail so
        it can be released, or False if the swap will happen at the next
        block instead. Without ``fade``, None.

    Raises:
        ValueError: The graph contains a file-backed source, or the sample
            was deinitialised.
    """

def reset(sample: _AudioSample, /) -> None:
    """Rewind a graph, as the pump does when a looping tail ends."""

def info(sample: _AudioSample, /) -> tuple[int, int, int, int, bool, bool]:
    """Describe a graph's output, so a sink can be opened to match it.

    Returns:
        ``(sample_rate, channel_count, bits_per_sample, max_buffer_length,
        samples_signed, single_buffer)``.
    """

def now() -> int:
    """Frames the pump has pulled since `spawn`.

    This is what an event's frame is compared against, so schedule with it.
    It wraps at 2**32 frames, which is 24.9 hours at 48 kHz, and `Events.at`
    takes a wrapped value.
    """

def events(queue: Events | None = ..., /) -> Events | None:
    """Attach or detach the queue the pump applies at block boundaries.

    Args:
        queue: An `Events` to attach, or None to detach. Leave the argument
            out to only read.

    Returns:
        The attached queue, or None.

    Raises:
        TypeError: ``queue`` is not an `Events` or None.
    """

def tap(tap: Tap | None = ..., /) -> Tap | None:
    """Attach or detach the `Tap` that copies what goes out.

    Args:
        tap: A `Tap` to attach, or None to detach. Leave the argument out to
            only read.

    Returns:
        The attached tap, or None.

    Raises:
        TypeError: ``tap`` is not a `Tap` or None.
    """

def threaded() -> bool:
    """Whether `spawn` puts the loop on a thread of its own. False in service
    mode, where `service` is the driver."""

def backpressure() -> bool:
    """Whether a full output ring makes the pump wait for room instead of
    dropping the block.

    This is True on a threaded port whose driver can end the wait with a wake,
    and in service mode, where the loop hands the thread back on a full ring.
    It is False only on a threaded build whose driver has no way to wake the
    pump, where a free-running pump would lose audio.
    """

def driver() -> str:
    """Which platform driver bound: ``"esp32"``, ``"pthread"``, ``"win32"`` or
    ``"none"``. ``"none"`` is a real answer, not a failure: one thread, no
    hardware, and `service` is the driver."""

def fades() -> tuple[int, int, int]:
    """What fading has done since `spawn`.

    Returns:
        ``(fades, clean_holds, silence_bytes)``: blocks that went out faded,
        fading parks that ended in time and were never heard, and the bytes
        of silence a held park fed the output.
    """

def fault() -> int:
    """Why the last pull gave up: 0 when it has not. The pull never raises,
    because there is no interpreter on its thread, so it publishes a code and
    stops. `lock_reset` clears it."""

def lock_stats() -> tuple[int, int, int, int, int, int, int]:
    """What the pump lock has cost since `spawn`.

    Returns:
        ``(pump_takes, pump_wait_us, pump_wait_us_max, ctrl_takes,
        ctrl_wait_us, ctrl_wait_us_max, ctrl_held_us_max)``: how often and how
        long the pump thread and the control calls waited for the lock.
    """

def lock_reset() -> None:
    """Zero the lock statistics and clear the fault code."""

class Ring:
    """A ring buffer Python writes PCM into, which is an audio sample.

    The pump pulls blocks out of it through the same protocol as any other
    node, so a pushed stream can be the tail by itself or sit behind a mixer
    with effects on it. One producer writes and one consumer pulls, and neither
    takes a lock to do it.

    An underrun plays a whole block of silence and counts it, leaving the audio
    in the ring to refill. An overrun makes `write` take what fits and count
    it. Neither raises or blocks.

    The audio is signed 16-bit.

    Args:
        sample_rate: Frames per second.
        channel_count: 1 or 2.
        frames: Frames in one block, the amount the pump pulls at a time.
        capacity: How many blocks deep the ring is, at least 2. Depth is
            latency.

    Raises:
        ValueError: An argument is out of range, or the ring would be over
            16 MB.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48000,
        channel_count: int = 2,
        frames: int = 256,
        capacity: int = 4,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def write(self, buffer: bytes | bytearray | memoryview, /) -> int:
        """Push PCM into the ring.

        Args:
            buffer: Signed 16-bit samples, interleaved.

        Returns:
            How many bytes were taken, in whole frames. It is less than the
            buffer's length when the ring is full, and the caller keeps the
            rest.
        """
    def space(self) -> int:
        """How many bytes `write` will take now, in whole frames."""
    def level(self) -> int:
        """How many bytes are queued."""
    def stats(self) -> tuple[int, int, int, int, int, int, int, int, int, int]:
        """Counters.

        Returns:
            ``(wrote, read, starved, underruns, overruns, pulls, in_digest,
            out_digest, capacity, block_size)``. The 64-bit values are only
            reliable once the pump has stopped; `level` and `space` are the
            live questions.
        """
    def clear(self) -> None:
        """Drop everything queued."""
    def from_usb(self, endpoint: int, packet: int | None, /) -> None:
        """Make a USB sound card's isochronous OUT endpoint write straight
        into this ring from the USB interrupt, so the pump plays it. Only on
        ports whose ``machine.USBDevice`` streams isochronous endpoints.

        While it runs the interrupt is the ring's one producer, so do not also
        `write`.

        Args:
            endpoint: The endpoint number.
            packet: The packet size in bytes, or None to stop.
        """
    def deinit(self) -> None:
        """Release the ring. Every later use raises `ValueError`."""

class DriftResampler:
    """Plays a `Ring` that something else fills on its own clock.

    It steers its resampling ratio so the ring holds `target` frames, which
    keeps the producer's clock and the output's clock from drifting apart. Play
    the resampler, not the ring: it is the ring's only reader. The ring's block
    must be 16 to 1024 frames.

    It is an audio sample, signed 16-bit, with the ring's channel count.

    Args:
        ring: The `Ring` to play.
        sample_rate: The output rate, from the ring's own rate (the default)
            up to twice it.
        target: The fill to hold, in frames. It must cover a block and fit in
            the ring less the filter's look-ahead of 16 frames. The default
            is half the ring.
        max_ppm: How far the ratio may move from nominal, in parts per
            million, 0 to 10000.
        bandwidth: How fast the loop follows the clocks once locked, in Hz,
            0.001 to 0.1. It starts eight times faster and narrows over about
            a minute. The default is 0.01.

    Raises:
        TypeError: ``ring`` is not a `Ring`.
        ValueError: An argument is out of range.
    """

    def __init__(
        self,
        ring: Ring,
        *,
        sample_rate: int | None = None,
        target: int | None = None,
        max_ppm: int = 1000,
        bandwidth: float | None = None,
    ) -> None: ...
    sample_rate: int
    """Frames per second of the output. Writable, to 1 or more."""
    target: int
    """The fill to hold, in frames. It can be set while playing; the fill
    moves there at the loop's pace. Setting it out of range raises
    `ValueError`."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def ppm(self) -> float:
        """The ratio's offset from nominal, in parts per million."""
    @property
    def fill(self) -> float:
        """The smoothed fill it is holding, in frames."""
    @property
    def rate(self) -> float:
        """Ring frames consumed per output frame."""
    @property
    def underruns(self) -> int:
        """Blocks of silence played because the ring ran dry, after which it
        waits for `target` again."""
    def reset(self) -> None:
        """Start over and forget the rate."""
    def deinit(self) -> None:
        """Release the resampler. Every later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Events:
    """A queue of frame-stamped operations the pump applies at block
    boundaries, so a sequenced bar keeps time on the audio clock instead of the
    interpreter's.

    Scheduling is a call on the interpreter thread and does every type and
    range check there; applying is a handful of stores on the pump's thread.
    A scheduled `synthio.Note` or sample stays alive until it is applied.
    Attach the queue with `events`.

    Args:
        capacity: How many operations the queue holds, 1 to 4096.

    Raises:
        ValueError: ``capacity`` is out of range.
    """

    def __init__(self, *, capacity: int = 64) -> None: ...
    def at(self, frame: int, op: int, target: object, arg: object = None, loop: bool = False, /) -> int:
        """Schedule an operation.

        Args:
            frame: When to apply it, in frames since `spawn`, as `now`
                counts. It is masked to 32 bits, so ``now() + n`` is fine
                across the wrap. Do not schedule more than about 12 hours
                ahead.
            op: The operation, and what ``target`` and ``arg`` are for it:

                - `PRESS` and `RELEASE`: ``target`` is a
                  `synthio.Synthesizer`, ``arg`` a `synthio.Note` or a MIDI
                  note number from 0 to 127.
                - `RELEASE_ALL`: ``target`` is a `synthio.Synthesizer`.
                - `PLAY`: ``target`` is an `audiomixer.MixerVoice`, ``arg`` a
                  sample whose format matches the mixer's. ``loop`` says
                  whether it loops.
                - `STOP`: ``target`` is an `audiomixer.MixerVoice`.
                - `LEVEL`: ``target`` is an `audiomixer.MixerVoice`, ``arg``
                  the level.
                - `STRIKE`: ``target`` is an `audiomodal.Bank`, ``arg`` an
                  ``array('f')`` of ``frequency, decay, gain`` for each mode.
                - `CHOKE`: ``target`` is an `audiomodal.Bank`.
            target: What the operation acts on.
            arg: The operation's argument.
            loop: Whether `PLAY` loops.

        Returns:
            A token for `cancel`, or 0 when the queue was full and the
            operation was dropped, which is counted rather than raised.

        Raises:
            TypeError: ``target`` or ``arg`` is the wrong type for ``op``.
            ValueError: ``op`` is unknown, a MIDI note is out of range, the
                voice is not in a mixer, or a strike's table is the wrong
                shape.
        """
    def cancel(self, token: int, /) -> bool:
        """Remove a scheduled operation that has not been applied.

        Returns:
            Whether the token was still queued.
        """
    def clear(self) -> int:
        """Remove everything queued, counting it as cancelled.

        Returns:
            How many operations were removed.
        """
    def pending(self) -> int:
        """How many operations are queued."""
    def stats(self) -> tuple[int, int, int, int, int, int, int, int]:
        """Counters.

        Returns:
            ``(scheduled, applied, late, cancelled, dropped, refused, depth,
            capacity)``: ``late`` operations were applied after their frame,
            ``dropped`` found the queue full and ``refused`` found their target
            gone when due.
        """

class Tap:
    """A copy of the audio going out, for a meter or a scope, without being in
    the path.

    The pump writes each block it sends into the tap's ring, and a reader takes
    the latest audio. Attach it with `tap`. The audio is signed 16-bit.

    Args:
        frames: How many frames to keep, at least 16. The size is rounded up
            to a power of two in bytes, to at most 4 MB.
        channel_count: 1 or 2.

    Raises:
        ValueError: An argument is out of range.
    """

    def __init__(self, *, frames: int = 2048, channel_count: int = 2) -> None: ...
    def readinto(self, buffer: bytearray | memoryview, /) -> int:
        """Copy the newest audio into a buffer.

        Args:
            buffer: A writable buffer. It is filled with whole frames, up to
                the tap's size.

        Returns:
            How many bytes were written. 0 when nothing has gone out since
            the last read, or when the pump kept overwriting the audio while
            it was copied, which `stats` counts.
        """
    def stats(self) -> tuple[int, int, int, int, int, int]:
        """Counters.

        Returns:
            ``(bytes_written, blocks, reads, torn, capacity, frame_size)``.
        """
