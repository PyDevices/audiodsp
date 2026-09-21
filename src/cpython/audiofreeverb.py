"""CircuitPython-compatible Freeverb streaming effect."""

from array import array

import _audiodsp

from audiofilters import _Effect
from audiofilters import _FilterChain
from audiofilters import _mix_down
from audiofilters import _sat16
from audiofilters import _value


class Freeverb(_Effect):
    _process_silence = True

    def __init__(self, *, roomsize=None, damp=None, pre_filter=None, post_filter=None, mix=None, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        if not samples_signed: raise ValueError("Freeverb requires signed samples")
        if bits_per_sample != 16: raise ValueError("bits_per_sample must be 16")
        self.roomsize = 0.5 if roomsize is None else roomsize
        self.damp = 0.5 if damp is None else damp
        self.mix = 0.5 if mix is None else mix
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        # CircuitPython allocates separate right-channel banks, but its
        # current processing loop resets the channel offsets for every
        # sample and therefore uses the first banks for both channels.
        self._comb = array("h", (0 for _ in range(11024)))
        self._comb_indices = array("I", (0 for _ in range(8)))
        self._comb_filters = array("h", (0 for _ in range(8)))
        self._allpass = array("h", (0 for _ in range(1563)))
        self._allpass_indices = array("I", (0 for _ in range(4)))
        self.pre_filter = pre_filter
        self.post_filter = post_filter

    def _reset_state(self):
        self._pre_filter.reset()
        self._post_filter.reset()

    @property
    def pre_filter(self):
        return self._pre_filter.obj

    @pre_filter.setter
    def pre_filter(self, value):
        self._pre_filter = _FilterChain(value, self.channel_count)

    @property
    def post_filter(self):
        return self._post_filter.obj

    @post_filter.setter
    def post_filter(self, value):
        self._post_filter = _FilterChain(value, self.channel_count)

    _COMB_SIZES = (1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617)
    _ALLPASS_SIZES = (556, 441, 341, 225)

    def _process_filtered(self, data, roomsize, damp, mix):
        samples = array("h")
        samples.frombytes(data)
        output = array("h", (0 for _ in range(len(samples))))
        n = len(samples)
        filter_channel = n % self.channel_count
        feedback = int(roomsize * 9175.04) + 22937
        damp1 = int(damp * 13107.2)
        damp2 = int(32768 - damp1)
        mix2 = mix * 2.0
        dry = int(((2.0 - mix2) if (2.0 - mix2) < 1.0 else 1.0) * 32767)
        wet = int((mix2 if mix2 < 1.0 else 1.0) * 32767)
        comb_offsets = []
        offset = 0
        for size in self._COMB_SIZES:
            comb_offsets.append(offset)
            offset += size
        allpass_offsets = []
        offset = 0
        for size in self._ALLPASS_SIZES:
            allpass_offsets.append(offset)
            offset += size
        for index, sample in enumerate(samples):
            filtered = self._pre_filter.process(filter_channel, sample)
            reverb_input = _sat16(int(filtered) * 8738, 17)
            total = 0
            for comb in range(8):
                pos = comb_offsets[comb] + self._comb_indices[comb]
                delayed = self._comb[pos]
                total += delayed
                self._comb_filters[comb] = _sat16(
                    delayed * damp2 + self._comb_filters[comb] * damp1, 15)
                self._comb[pos] = _sat16(
                    reverb_input + _sat16(self._comb_filters[comb] * feedback, 15), 0)
                nxt = self._comb_indices[comb] + 1
                self._comb_indices[comb] = 0 if nxt >= self._COMB_SIZES[comb] else nxt
            effect = _sat16(total * 31457, 17)
            for allpass in range(4):
                pos = allpass_offsets[allpass] + self._allpass_indices[allpass]
                delayed = self._allpass[pos]
                self._allpass[pos] = (
                    (effect + (delayed >> 1) + 32768) & 0xffff) - 32768
                effect = _sat16(delayed - effect, 1)
                nxt = self._allpass_indices[allpass] + 1
                self._allpass_indices[allpass] = (
                    0 if nxt >= self._ALLPASS_SIZES[allpass] else nxt)
            effect = self._post_filter.process(filter_channel, effect)
            word = effect * 30
            word = _sat16(sample * dry, 15) + _sat16(word * wet, 15)
            output[index] = _mix_down(word)
        return output.tobytes()

    def _process(self, data):
        if not data:
            return data
        from synthio import _advance_blocks
        _advance_blocks(self.sample_rate,
                        len(data) // (self.channel_count * 2))
        roomsize = min(1.0, max(0.0, _value(self.roomsize)))
        damp = min(1.0, max(0.0, _value(self.damp)))
        mix = min(1.0, max(0.0, _value(self.mix)))
        if self._pre_filter or self._post_filter:
            self._pre_filter.tick(self.sample_rate)
            self._post_filter.tick(self.sample_rate)
            return self._process_filtered(data, roomsize, damp, mix)
        return _audiodsp.freeverb_s16(
            data, self._comb, self._comb_indices, self._comb_filters,
            self._allpass, self._allpass_indices, roomsize, damp, mix,
        )


__all__ = ("Freeverb",)
