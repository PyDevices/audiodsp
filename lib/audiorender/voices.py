"""What a render pulls audio from, and how it tells them the time.

A *voice* is anything a render can drive: it takes the events
:mod:`audiorender.events` builds and hands back frames.

    deliver(event, sample_position)
    pull_frames(frames) -> interleaved little-endian stereo int16

:class:`Voice` is that protocol over an :mod:`audioinstruments` instrument,
which is what a caller wants unless it has its own way of loading a sound -
a plug-in host driving its own script runner, say. Keeping the protocol
this small is what lets such a host reuse the render loop without reusing
the loader.
"""

import audiocore

from .events import deliver


class Puller:
    """Frames from an audiodsp node, in whatever size the caller asks for.

    A node hands out buffers of its own choosing, so a render that wants a
    fixed block has to keep the remainder between calls.
    """

    def __init__(self, node):
        self.node = node
        self._pending = b""

    def pull_frames(self, frames):
        need = frames * 4
        while len(self._pending) < need:
            _, view = audiocore.get_buffer(self.node)
            chunk = bytes(view)
            if not chunk:
                # A finished source is silence, not the end of the render:
                # a track keeps its place in the mix after its last note.
                chunk = b"\x00" * 1024
            self._pending += chunk
        out = self._pending[:need]
        self._pending = self._pending[need:]
        return out


class Voice(Puller):
    """One :mod:`audioinstruments` instrument, ready to be rendered."""

    def __init__(self, instrument):
        Puller.__init__(self, instrument.output)
        self.instrument = instrument

    def deliver(self, event, sample_position):
        deliver(self.instrument, event, sample_position)


class Clock:
    """The transport reading a render publishes, one block at a time.

    Instruments that lock to tempo - a synced echo, a sidechain pump - take
    a `transport` callable and read it while they render. Pass one of these
    as that callable and give the same object to the render loop, which
    moves it to the head of each block.
    """

    def __init__(self, tempo):
        self.tempo = tempo
        self.reading = tempo.transport_at(0)

    def __call__(self):
        return self.reading

    def move_to(self, sample):
        self.reading = self.tempo.transport_at(sample)


class PcmSource:
    """A finite audiodsp source over interleaved stereo int16 `pcm`.

    An effect rack reads a track that has already been rendered, so the
    track goes back in as a source. Handed out in chunks rather than in one
    buffer because a song is tens of megabytes and every node downstream
    would otherwise be asked to hold all of it at once.
    """

    def __init__(self, pcm, sample_rate, channel_count=2, chunk_bytes=2048):
        self._pcm = bytes(pcm)
        self._position = 0
        self._chunk_bytes = int(chunk_bytes)
        self.sample_rate = int(sample_rate)
        self.channel_count = int(channel_count)
        self.bits_per_sample = 16
        self.samples_signed = True

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._position = 0

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        start = self._position
        end = min(len(self._pcm), start + self._chunk_bytes)
        self._position = end
        status = 0 if end >= len(self._pcm) else 1
        return status, memoryview(self._pcm[start:end])
