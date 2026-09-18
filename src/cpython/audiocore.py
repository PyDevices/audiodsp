"""CircuitPython-compatible audio sample sources for CPython."""

import wave

import _audioif  # noqa: F401 - verifies that the native runtime is present

GET_BUFFER_DONE = 0
GET_BUFFER_MORE_DATA = 1
GET_BUFFER_ERROR = 2


#: What a released node raises, on **every** target: CircuitPython's own
#: `ValueError`, with CircuitPython's own message. It comes from
#: `shared-bindings/util.c`, which `src/cp_compat/util.c` ports verbatim for the
#: native builds; `_audioif`'s guard (`rawsample_raise_status`) and this shim
#: now match it rather than raising `RuntimeError`.
#:
#: The exception type is the one thing about a released node that portable user
#: code can actually catch, and the parity gates cannot see it because they
#: compare rendered bytes. A `try/except ValueError` around a teardown path has
#: to work the same on a board and on this target. audioif#73.
DEINITED_MESSAGE = ("Object has been deinitialized and can no longer be used. "
                    "Create a new object.")


def raise_deinited_error():
    raise ValueError(DEINITED_MESSAGE)


class _AudioSample:
    def __getattribute__(self, name):
        if not name.startswith("_") and name != "deinit":
            namespace = object.__getattribute__(self, "__dict__")
            if namespace.get("_deinited", False):
                raise_deinited_error()
        return object.__getattribute__(self, name)

    def _check(self):
        if getattr(self, "_deinited", False):
            raise_deinited_error()

    def deinit(self):
        if object.__getattribute__(self, "__dict__").get("_deinited", False):
            return
        self._release()
        self._deinited = True

    def _release(self):
        pass

    def _publish(self, data, slots=1):
        """Hand back the node's OWN output buffer, refilled.

        A native node renders into a buffer that belongs to it -- `int16_t
        buffer[AUDIOIF_..._FRAMES * 2]` in `src/<module>/<Node>.h` -- and
        `audiosample_get_buffer` hands back a pointer to it. A consumer that
        keeps that pointer across calls therefore reads what the node
        rendered LAST, not what it had rendered when the pointer was taken.
        `audiomixer.MixerVoice` is exactly such a consumer: `play()` fetches
        one block and the mix-down reads it later, from the pointer.

        This side used to answer every pull with a fresh `bytes`, so a
        borrowed block was a snapshot and could not be overtaken. A class
        that pulls a node one of its own mixer voices is already holding --
        `audioeffects.rebuilt.Saturation._charge_coupling` settling a
        coupling pole before the first block -- therefore rendered its first
        block differently here than on any native build (audioif#89).

        `slots` is how many buffers the native node rotates through: one for
        the nodes audioif wrote itself, two for the ported CircuitPython
        effects (`int8_t *buffer[2]`), for `Mixer` (`first_buffer` /
        `second_buffer`) and for the synthesizer.
        """
        buffers = getattr(self, "_out_buffers", None)
        if buffers is None or len(buffers) != slots:
            buffers = [bytearray(len(data)) for _ in range(slots)]
            self._out_buffers = buffers
            self._out_slot = 0
        index = self._out_slot
        self._out_slot = index + 1 if index + 1 < slots else 0
        buffer = buffers[index]
        if len(buffer) != len(data):
            # A node whose block size varies -- `audiospeed.Resampler`,
            # `audioshaper.SampleHold` -- takes a new buffer rather than a
            # resize, because a resize raises while a borrower holds a view.
            buffers[index] = buffer = bytearray(len(data))
        buffer[:] = data
        return memoryview(buffer)

    def __enter__(self):
        self._check()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.deinit()


RawSample = _audioif.RawSample


class WaveFile(_AudioSample):
    def __init__(self, file, buffer=None):
        self._file_owner = file
        self._stream = open(file, "rb") if isinstance(file, (str, bytes)) else file
        self._close_stream = self._stream is not file
        self._wave = wave.open(self._stream, "rb")
        self.channel_count = self._wave.getnchannels()
        self.sample_rate = self._wave.getframerate()
        self.bits_per_sample = self._wave.getsampwidth() * 8
        self.samples_signed = self.bits_per_sample != 8
        self._buffer_size = memoryview(buffer).nbytes if buffer is not None else 1024
        self._buffer_owner = buffer
        self._deinited = False

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        self._wave.rewind()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        frame_size = self.channel_count * self.bits_per_sample // 8
        data = self._wave.readframes(max(1, self._buffer_size // frame_size))
        if not data:
            return GET_BUFFER_DONE, memoryview(b"")
        result = GET_BUFFER_DONE if self._wave.tell() >= self._wave.getnframes() else GET_BUFFER_MORE_DATA
        # Two, as `audiocore/WaveFile.h`'s `buffer` / `second_buffer` are.
        return result, self._publish(data, 2)

    def _release(self):
        wave_reader = getattr(self, "_wave", None)
        self._wave = self._buffer_owner = self._file_owner = None
        if wave_reader is not None:
            wave_reader.close()
        if getattr(self, "_close_stream", False) and self._stream is not None:
            self._stream.close()
        self._stream = None


def _sample_method(sample, name):
    method = getattr(sample, name, None)
    if method is None:
        raise TypeError("object does not implement the audiocore sample protocol")
    return method


def reset_buffer(sample, single_channel_output=False, audio_channel=0):
    _sample_method(sample, "_reset_buffer")(single_channel_output, audio_channel)


def get_buffer(sample, single_channel_output=False, audio_channel=0):
    result, data = _sample_method(sample, "_get_buffer")(single_channel_output, audio_channel)
    # Deliberately own a byte-format copy: callers may retain it after the
    # producer advances or is deinitialized. `src/audiocore/module.c` copies
    # here too, and for the same reason ("the gc semantics of get_buffer are
    # unclear"), so the two targets hand a script the same thing.
    return int(result), memoryview(bytes(data))


def _borrow(sample, single_channel_output=False, audio_channel=0):
    """Pull `sample` and keep the buffer it produced, without copying it.

    What one node does to another inside the graph. `audiosample_get_buffer`
    hands the native caller a pointer into the producer's own buffer, so a
    consumer that holds it reads the producer's latest render, not the one it
    asked for -- see `_AudioSample._publish`. `get_buffer()` above is the
    script-facing entry point and copies, on this target and on the native
    builds both; this is the in-graph one and does not.
    """
    result, data = _sample_method(sample, "_get_buffer")(single_channel_output, audio_channel)
    # Always a byte view, the same rule `src/audiocore/module.c` states for
    # `get_buffer()`: len() is the length in BYTES, whatever the producer's
    # own format was.
    return int(result), memoryview(data).cast("B")


def get_structure(sample, single_channel_output=False):
    return {
        "sample_rate": sample.sample_rate,
        "bits_per_sample": sample.bits_per_sample,
        "channel_count": 1 if single_channel_output else sample.channel_count,
        "samples_signed": sample.samples_signed,
    }


__all__ = ("RawSample", "WaveFile", "get_buffer", "reset_buffer", "get_structure")
