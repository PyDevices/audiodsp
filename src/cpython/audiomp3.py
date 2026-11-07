# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""MP3 files as audio samples, for CPython, as CircuitPython's ``audiomp3``.

The decoding is the Helix fixed-point decoder the boards run
(``_audiodsp.MP3Stream``), and the buffering around it follows the board's
``MP3Decoder`` step for step, so a file decodes to the same bytes here as on
a board.
"""

import _audiodsp

from audiocore import _AudioSample

__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

if not hasattr(_audiodsp, "MP3Stream"):
    raise ImportError("audiomp3: this build of _audiodsp was made without the MP3 decoder")

#: Bytes in the input buffer, as ``DEFAULT_INPUT_BUFFER_SIZE`` in
#: ``src/audiomp3/MP3Decoder.c``.
_INPUT_BYTES = 2048
#: The largest decoded frame: 1152 samples on each of two channels, 16 bits.
_MAX_BUFFER_LEN = 1152 * 2 * 2


class MP3Decoder(_AudioSample):
    """Load an MP3 file for playback.

    ``file`` is a file opened in binary mode, or a path. ``buffer`` is
    optional: on a board it is the memory the decoder works in. Here only its
    size matters: one larger than 11264 bytes gives the decoder a larger input
    buffer, as it does there.
    """

    def __init__(self, file, buffer=None):
        self._input_bytes = _INPUT_BYTES
        if buffer is not None:
            size = memoryview(buffer).nbytes
            if size > _INPUT_BYTES + 2 * _MAX_BUFFER_LEN:
                self._input_bytes = size - 2 * _MAX_BUFFER_LEN
        self._buffer_owner = buffer
        self._decoder = None
        self._stream = None
        self._close_stream = False
        self._deinited = False
        owned = isinstance(file, (str, bytes))
        self._set_file(open(file, "rb") if owned else file, owned=owned)

    def _set_file(self, stream, owned=False):
        if getattr(stream, "encoding", None) is not None:
            raise TypeError("file must be a file opened in byte mode")
        decoder = _audiodsp.MP3Stream(stream, self._input_bytes)
        rate, channels, buffer_bytes = decoder.info()
        old_decoder, old_stream, old_owned = self._decoder, self._stream, self._close_stream
        self._decoder = decoder
        self._stream = stream
        self._close_stream = owned
        self.sample_rate = rate
        self.channel_count = channels
        self.bits_per_sample = 16
        self.samples_signed = True
        self._buffer_bytes = buffer_bytes
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
        """How loud the last decoded frame was."""
        return self._decoder.rms_level()

    @property
    def samples_decoded(self):
        """Samples decoded since the start, every channel counted."""
        return self._decoder.samples_decoded()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        if single_channel_output and audio_channel == 1:
            return
        self._decoder.rewind()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        result, data = self._decoder.decode()
        # Two, as MP3Decoder.h's pcm_buffer[2] are.
        return result, self._publish(data, 2)

    def _release(self):
        decoder, self._decoder = self._decoder, None
        if decoder is not None:
            decoder.close()
        if self._close_stream and self._stream is not None:
            self._stream.close()
        self._stream = None
        self._buffer_owner = None


__all__ = ("MP3Decoder",)
