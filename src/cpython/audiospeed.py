"""CircuitPython-compatible streaming speed changer."""

from audiocore import GET_BUFFER_DONE, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer, reset_buffer


class SpeedChanger(_AudioSample):
    def __init__(self, source, rate=None):
        self.source = source
        self.sample_rate, self.channel_count = source.sample_rate, source.channel_count
        self.bits_per_sample, self.samples_signed = source.bits_per_sample, source.samples_signed
        self._rate_fp = 1 << 16
        if rate is not None: self.rate = rate
        self._phase = 0
        self._source_data = None
        self._source_done = self._source_exhausted = False
        self._deinited = False

    @property
    def rate(self): return self._rate_fp / 65536.0

    @rate.setter
    def rate(self, value):
        value = float(value)
        if not 0 <= value <= 1000: raise ValueError("rate must be from 0 to 1000")
        # Rounded, not truncated: upstream's cast loses a whole Q16 LSB for a
        # float a hair under its neighbour. docs/upstream-diff.md, audiodsp#92.
        self._rate_fp = int(value * 65536 + 0.5) & 0xffffffff

    def _release(self):
        self.source = None
        self._source_data = None
    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        if single_channel_output and audio_channel == 1: return
        reset_buffer(self.source, False, 0)
        self._phase = 0
        self._source_data = None
        self._source_done = self._source_exhausted = False

    def _frame_size(self):
        return self.bits_per_sample // 8 * self.channel_count

    def _fetch(self):
        if self._source_exhausted: return False
        result, data = get_buffer(self.source, False, 0)
        raw = bytes(data)
        if result == 2 or len(raw) < self._frame_size():
            self._source_exhausted = True
            return False
        # Carry the accumulator across the boundary, rather than zeroing it:
        # what this buffer consumed is the frame count of the one before it.
        # audiodsp#91, docs/upstream-diff.md.
        consumed = (len(self._source_data or b"") // self._frame_size()) << 16
        self._phase = self._phase - consumed if self._phase >= consumed else 0
        self._source_data = raw
        self._source_done = result == GET_BUFFER_DONE
        return True

    def _advance_to_phase(self):
        """Pull until the frame the accumulator names lands inside a buffer."""
        while self._phase >> 16 >= len(self._source_data) // self._frame_size():
            if self._source_done:
                self._source_exhausted = True
                return False
            if not self._fetch():
                return False
        return True

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        if self._source_data is None and not self._fetch():
            return GET_BUFFER_DONE, memoryview(b"")
        frame_size = self._frame_size()
        output = bytearray()
        while len(output) < 128 * frame_size:
            if not self._advance_to_phase(): break
            start = (self._phase >> 16) * frame_size
            output += self._source_data[start:start + frame_size]
            self._phase = (self._phase + self._rate_fp) & 0xffffffff
        result = GET_BUFFER_DONE if self._source_exhausted else GET_BUFFER_MORE_DATA
        return result, self._publish(output)


class Resampler(SpeedChanger):
    def __init__(self, source):
        super().__init__(source)
        self._destination_rate = 0

    @property
    def rate(self):
        return self._rate_fp / 65536.0

    def _bind_sample_rate(self, sample_rate):
        self._destination_rate = sample_rate
        if self.source is not None and sample_rate:
            self._rate_fp = int(self.sample_rate / sample_rate * 65536 + 0.5) & 0xffffffff
        else:
            self._rate_fp = 1 << 16


__all__ = ("SpeedChanger", "Resampler")
