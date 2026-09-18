"""Route audio: fan one stream out to parallel branches, or move it between
the channels.

Unlike the rest of this package, `audioroute` is not a CircuitPython module.
`Splitter` comes from micropython-vst3's `vstaudio` engine, where the effects
library's exciters, Haas wideners and multiband splits are built on it.
`MidSide` is audioif's own and has no ancestor anywhere: it turns a stereo
pair into its mono sum and its difference, scales the difference, and rebuilds
the pair, which is how a stereo drive keeps its image and the only way this
palette collapses a pair to mono or pushes its sides out.

    split = audioroute.Splitter(source, taps=3)
    low.play(split.tap(0))
    mid.play(split.tap(1))
    high.play(split.tap(2))
    mixer.play(low, voice=0) ...

Every tap reads the same stream at its own pace over a shared ring. Whichever
one is pulled first refills the ring; the others read what it wrote. A branch
that nobody reads must not wedge the ring, so writing past a laggard's cursor
drags it forward: that branch skips ahead rather than stalling the graph.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
    raise_deinited_error,
)
import _audioif


#: The audioif this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audioif_build.h). audioif#55.
__version__ = _audioif.__version__
__revision__ = _audioif.__revision__

MAX_TAPS = 4
CHUNK_FRAMES = _audioif.SPLITTER_CHUNK_FRAMES
#: How many frames the shared ring holds. A source may hand back more
#: than this in one go; the Splitter writes it in ring-sized pieces
#: rather than lapping its own readers. audioif#87.
RING_FRAMES = _audioif.SPLITTER_RING_FRAMES

_SILENCE = bytes(CHUNK_FRAMES * 4)


class SplitterTap(_AudioSample):
    """One branch's view of a Splitter's ring. Built by the Splitter."""

    def __init__(self, owner, index, sample_rate, channel_count):
        self._owner = owner
        self._index = index
        self.sample_rate = sample_rate
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = CHUNK_FRAMES * 2 * channel_count
        self._deinited = False

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        # `_check()` first, then deliberately nothing. The cursors belong to
        # the Splitter and the other taps are still reading against them;
        # rewinding one branch mid-stream would desynchronise the rest. But a
        # *released* tap must refuse rather than quietly succeed: the guard on
        # this target lives in each `_reset_buffer`/`_get_buffer`, because
        # `_AudioSample.__getattribute__` lets underscore names through, and
        # a body of bare `pass` was the one that never asked. On the native
        # builds `audiosample_reset_buffer` guards this for every type at
        # once.
        self._check()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        owner = self._owner
        if owner._ring.starved(self._index):
            owner._pull()
        data = owner._ring.take(self._index)
        if not data:
            # Still nothing: the source is dry, or another tap has already
            # read past what one pull could supply. A buffer of this tap's
            # own, where the native hands out the Splitter's shared
            # `silence` -- one zeroed block either way, so a borrower reads
            # the same bytes; see `audiocore._AudioSample._publish`.
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(CHUNK_FRAMES * 2 * self.channel_count))
        # Not published: the ring IS this tap's storage, exactly as the
        # native hands back `&state.ring[start * 2]`, and a second take
        # lands on the next region rather than rewriting this one.
        return GET_BUFFER_MORE_DATA, memoryview(data)


class Splitter:
    def __init__(self, source, taps=2):
        taps = int(taps)
        if taps < 1 or taps > MAX_TAPS:
            raise ValueError("taps must be 1..4")
        self._source = source
        self.channel_count = int(source.channel_count)
        if self.channel_count not in (1, 2):
            raise ValueError("source channel_count must be 1 or 2")
        self._ring = _audioif.SplitterRing(
            taps=taps, channel_count=self.channel_count)
        self._tap_count = taps
        # Every tap exists from the start, whether or not anything asks for
        # it: the ring drops what an unread tap never collects, so a branch
        # built late would begin mid-stream rather than at the beginning.
        self._taps = tuple(SplitterTap(self, index, source.sample_rate,
                                       self.channel_count)
                           for index in range(taps))
        self._deinited = False
        #: What one pull from the source did not fit in the ring, offered
        #: before the source is asked again. audioif#87.
        self._pending = b""

    def tap(self, index):
        if self._deinited:
            raise_deinited_error()
        index = int(index)
        if index < 0 or index >= self._tap_count:
            raise ValueError("tap index out of range")
        return self._taps[index]

    def deinit(self):
        """Release the branch.

        Not `_AudioSample.deinit`: a Splitter is not a sample, it hands out
        taps, so it keeps its own flag. It releases the upstream chain by
        dropping the source, and it releases every tap, because the ring the
        taps read belongs to this object - a tap outliving it would be
        reading a ring nothing refills. The ring itself goes when the last
        reference to this object does; there is no separate allocation to
        hand back.
        """
        if self._deinited:
            return
        self._deinited = True
        for tap in self._taps:
            tap.deinit()
        self._taps = ()
        self._source = None
        self._ring = None
        self._pending = b""

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.deinit()

    def _pull(self):
        if self._deinited or self._source is None:
            return
        # WHAT THE LAST PULL COULD NOT FIT COMES FIRST. A source hands back
        # what it has -- a RawSample over a 9600-frame table returns all 9600
        # in one call -- and the ring holds 8192. Writing the lot laps every
        # cursor including the one about to read, so the head is destroyed
        # unseen and the stream has a seam at 8192. `write` takes one ring's
        # worth and says how much; this holds the rest, and the source is not
        # asked again until it is gone. audioif#87.
        if not self._pending:
            result, data = get_buffer(self._source, False, 0)
            if result == GET_BUFFER_ERROR:
                return
            self._pending = bytes(data)
            if not self._pending:
                return
        taken = self._ring.write(self._pending)
        self._pending = self._pending[taken * 2 * self.channel_count:]


MIDSIDE_FRAMES = _audioif.MIDSIDE_FRAMES


class MidSide(_AudioSample):
    """Scale the difference between the channels, leaving the sum alone.

    ``width=0`` collapses the pair to mono, ``1`` passes it through
    untouched, ``2`` doubles the sides. The identity at ``width=1`` is
    exact - the output bytes are the input bytes, for every int16 pair -
    so the node costs nothing to leave in a chain that is not using it.
    """

    def __init__(self, source=None, width=1.0, sample_rate=48000,
                 channel_count=2):
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = MIDSIDE_FRAMES * 2 * channel_count
        self._deinited = False
        self._source = source
        self._width = 1.0
        self._pending = b""
        self._apply({"width": width})

    def _apply(self, options):
        for name, value in options.items():
            if name != "width":
                raise TypeError("unknown MidSide option %r" % (name,))
            self._width = min(2.0, max(0.0, float(value)))

    def set(self, **options):
        """Change settings mid-stream."""
        self._check()
        self._apply(options)

    @property
    def playing(self):
        return self._source is not None

    def play(self, sample, *, loop=False):
        """Set the source the matrix reads from."""
        self._check()
        self._source = sample
        self._pending = b""

    def stop(self):
        self._source = None
        self._pending = b""

    def _release(self):
        self.stop()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        # The cursor is all there is to reset: the matrix carries no state
        # between frames.
        self._pending = b""

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        output = bytearray()
        produced = 0
        width = 2 * self.channel_count
        while produced < MIDSIDE_FRAMES:
            if not self._pending:
                if self._source is None:
                    break
                result, data = get_buffer(self._source, False, 0)
                data = bytes(data)
                if result == GET_BUFFER_ERROR or len(data) < width:
                    break
                self._pending = data[:len(data) // width * width]
            run = min(MIDSIDE_FRAMES - produced, len(self._pending) // width)
            output += _audioif.midside_s16(
                self._pending[:run * width], self._width, self.channel_count)
            self._pending = self._pending[run * width:]
            produced += run
        # A starved chain gets silence rather than a short block: this node
        # sits in the middle of a live graph and never reports itself
        # finished.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(MIDSIDE_FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("MidSide", "Splitter", "SplitterTap")
