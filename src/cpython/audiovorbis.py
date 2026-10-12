"""Ogg Vorbis files as audio samples, for CPython.

Not part of CircuitPython, which has no Vorbis module: this is PyDevices'.
``VorbisDecoder`` is shaped like ``audiomp3.MP3Decoder`` - the same
constructor, properties and methods - so either plays wherever the other
does. The decoding is Xiph's Tremor, the same C the boards run
(``_audiodsp.VorbisStream``), so a file decodes to the same samples here as
on a board.
"""

import _audiodsp

from audiocore import GET_BUFFER_DONE, GET_BUFFER_MORE_DATA, _AudioSample

__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

#: Frames in each of the two output buffers, as ``AUDIOVORBIS_FRAMES`` in
#: ``src/audiovorbis/VorbisDecoder.h``.
_FRAMES = 1024
#: The optional buffer is used when each half holds at least this much.
_MIN_HALF_BYTES = 256


class VorbisDecoder(_AudioSample):
    """Load an Ogg Vorbis file for playback.

    ``file`` is a file opened in binary mode, or a path. ``buffer`` is
    optional: when given, and each half holds at least 256 bytes, it sets how
    much audio each pull hands back (half of it, in whole stereo frames).
    Mono and stereo files play; a file with more channels is refused.
    """

    def __init__(self, file, buffer=None):
        self._frames = _FRAMES
        if buffer is not None:
            half = (memoryview(buffer).nbytes // 2) & ~3
            if half >= _MIN_HALF_BYTES:
                self._frames = half // 4
        self._buffer_owner = buffer
        self._decoder = None
        self._stream = None
        self._close_stream = False
        self._deinited = False
        self._set_file(open(file, "rb") if isinstance(file, (str, bytes)) else file,
                       owned=isinstance(file, (str, bytes)))

    def _set_file(self, stream, owned=False):
        if getattr(stream, "encoding", None) is not None:
            raise TypeError("file must be a file opened in byte mode")
        decoder = _audiodsp.VorbisStream(stream)
        rate, channels, _ = decoder.info()
        if channels > 2:
            decoder.close()
            raise ValueError("only mono and stereo Vorbis files can play")
        old_decoder, old_stream, old_owned = self._decoder, self._stream, self._close_stream
        self._decoder = decoder
        self._stream = stream
        self._close_stream = owned
        self.sample_rate = rate
        self.channel_count = channels
        self.bits_per_sample = 16
        self.samples_signed = True
        self._eof = False
        self._started = False
        self._samples_decoded = 0
        self._last = b""
        if old_decoder is not None:
            old_decoder.close()
        if old_owned and old_stream is not None and old_stream is not stream:
            old_stream.close()

    @property
    def file(self):
        """The file the decoder is playing. Setting it starts a new one."""
        return self._stream

    @file.setter
    def file(self, stream):
        self._check()
        self._set_file(stream)

    def open(self, filepath):
        """Play the file at ``filepath`` instead."""
        self._check()
        self._set_file(open(filepath, "rb"), owned=True)

    @property
    def rms_level(self):
        """How loud the last buffer was, as ``MP3Decoder.rms_level`` measures it."""
        data = memoryview(self._last).cast("h") if self._last else ()
        if not data:
            return 0.0
        return sum(v * v for v in data) ** 0.5 / len(data)

    @property
    def samples_decoded(self):
        """Samples handed out since the start, every channel counted."""
        return self._samples_decoded

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        if single_channel_output and audio_channel == 1:
            return
        if not self._started:
            return
        self._decoder.seek(0)
        self._eof = False
        self._started = False
        self._samples_decoded = 0

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        data = b""
        if not self._eof:
            data = self._decoder.read(self._frames)
            if len(data) < self._frames * 2 * self.channel_count:
                self._eof = True
            if data:
                self._started = True
        self._last = data
        self._samples_decoded += len(data) // 2
        result = GET_BUFFER_DONE if self._eof else GET_BUFFER_MORE_DATA
        # Two, as VorbisDecoder.h's pcm_buffer[2] are.
        return result, self._publish(data, 2)

    def _release(self):
        decoder, self._decoder = self._decoder, None
        if decoder is not None:
            decoder.close()
        if self._close_stream and self._stream is not None:
            self._stream.close()
        self._stream = None
        self._buffer_owner = None


__all__ = ("VorbisDecoder",)
