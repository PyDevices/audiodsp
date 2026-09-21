"""CircuitPython-compatible streaming audio filters."""

from array import array
from enum import Enum
import math
import struct
from audiocore import GET_BUFFER_MORE_DATA, _AudioSample, get_buffer, reset_buffer
import _audiodsp


def _value(value):
    evaluator = getattr(value, "_evaluate", None)
    if evaluator is not None:
        # Import lazily: synthio imports audiocore and effects may in turn
        # be imported while applications are assembling their graph.
        from synthio import _BLOCK_TICK
        return float(evaluator(_BLOCK_TICK))
    return float(value.value if hasattr(value, "value") else value)


class _CircuitEnum(Enum):
    def __repr__(self):
        return "{}.{}.{}".format(
            self.__class__.__module__, self.__class__.__name__, self.name)

    __str__ = __repr__


class DistortionMode(_CircuitEnum):
    CLIP = 0
    LOFI = 1
    OVERDRIVE = 2
    WAVESHAPE = 3


class _Effect(_AudioSample):
    _process_during_pull = True

    def _init_format(self, *, buffer_size, sample_rate, bits_per_sample, samples_signed, channel_count):
        if channel_count not in (1, 2): raise ValueError("channel_count must be 1 or 2")
        if bits_per_sample not in (8, 16): raise ValueError("bits_per_sample must be 8 or 16")
        if sample_rate < 1: raise ValueError("sample_rate must be at least 1")
        self.buffer_size, self.sample_rate = int(buffer_size), int(sample_rate)
        self.bits_per_sample, self.samples_signed = int(bits_per_sample), bool(samples_signed)
        self.channel_count, self._sample, self._loop, self._deinited = int(channel_count), None, False, False
        self._remaining, self._source_more = b"", False

    @property
    def playing(self): return self._sample is not None
    def play(self, sample, *, loop=False):
        self._check()
        from audiospeed import Resampler
        if isinstance(sample, Resampler):
            for name in ("channel_count", "bits_per_sample", "samples_signed"):
                if getattr(sample, name) != getattr(self, name): raise ValueError("The sample's %s does not match" % name)
            sample._bind_sample_rate(self.sample_rate)
        else:
            for name in ("sample_rate", "channel_count", "bits_per_sample", "samples_signed"):
                if getattr(sample, name) != getattr(self, name): raise ValueError("The sample's %s does not match" % name)
        self._sample, self._loop = sample, bool(loop); reset_buffer(sample)
        result, data = get_buffer(sample)
        self._remaining = bytes(data)
        self._source_more = result == GET_BUFFER_MORE_DATA
    def stop(self): self._sample = None
    def _release(self):
        self._sample = None
        self._remaining = b""
    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        self._reset_state()
    def _reset_state(self):
        pass
    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        output = bytearray()
        while len(output) < self.buffer_size and self._sample is not None:
            take = min(self.buffer_size - len(output), len(self._remaining))
            if self._process_during_pull:
                take = min(take, 256 * self.channel_count *
                           (self.bits_per_sample // 8))
            segment = bytes(self._remaining[:take])
            output += self._process(segment) if self._process_during_pull else segment
            self._remaining = self._remaining[take:]
            if len(output) == self.buffer_size:
                break
            if not self._remaining:
                if not self._source_more:
                    if self._loop:
                        reset_buffer(self._sample, single_channel_output, audio_channel)
                    else:
                        self._sample = None
                        break
                if self._sample is not None:
                    result, data = get_buffer(
                        self._sample, single_channel_output, audio_channel
                    )
                    self._remaining = bytes(data)
                    self._source_more = result == GET_BUFFER_MORE_DATA
                    if not self._remaining and not self._source_more:
                        self._sample = None
                        break
        valid_length = len(output)
        output += bytes(self.buffer_size - valid_length)
        if self._process_during_pull:
            if getattr(self, "_process_silence", False) and valid_length < self.buffer_size:
                output[valid_length:] = self._process(
                    bytes(self.buffer_size - valid_length))
            processed = bytes(output)
        elif getattr(self, "_process_silence", False):
            processed = self._process(bytes(output))
        else:
            processed = (self._process(bytes(output[:valid_length]))
                         + bytes(self.buffer_size - valid_length))
        # Two, as `audiofilters/Filter.h`'s `int8_t *buffer[2]` is -- the
        # shape every ported CircuitPython effect shares.
        return GET_BUFFER_MORE_DATA, self._publish(processed, 2)

    def _process(self, data):
        return data


class Filter(_Effect):
    # The C implementation filters each at-most-256-frame source segment
    # before requesting the next one.  This ordering matters when both the
    # source and filter parameters are driven by synthio blocks.
    def __init__(self, *, filter=None, mix=1, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        self.filter, self.mix = filter, mix
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        if filter is None:
            self._filters = ()
        elif isinstance(filter, (tuple, list)):
            self._filters = tuple(filter)
        else:
            self._filters = (filter,)
        self._filter_states = tuple(_audiodsp.BiquadState() for _ in self._filters)

    def _reset_state(self):
        for state in self._filter_states:
            state.reset()

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data or not self._filters:
            return data
        from synthio import _advance_blocks
        frame_count = len(data) // (self.channel_count * 2)
        _advance_blocks(self.sample_rate, frame_count)
        requested_mix = min(1.0, max(0.0, _value(self.mix)))
        result = data
        for index, (biquad, state) in enumerate(zip(self._filters, self._filter_states)):
            stage_mix = requested_mix if index == len(self._filters) - 1 else 1.0
            amplitude = 1.0 if biquad.A is None else _value(biquad.A)
            result = state.process_s16(
                result, biquad.mode.value, _value(biquad.frequency),
                _value(biquad.Q), amplitude, self.sample_rate, stage_mix,
                self.channel_count,
            )
        return result


class Distortion(_Effect):
    def __init__(self, *, drive=0, pre_gain=0, post_gain=0, mode=DistortionMode.CLIP, soft_clip=False, mix=1, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        if not isinstance(mode, DistortionMode): raise TypeError("mode must be a DistortionMode")
        self.drive, self.pre_gain, self.post_gain = drive, pre_gain, post_gain
        self.mode, self.soft_clip, self.mix = mode, bool(soft_clip), mix
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data:
            return data
        from synthio import _advance_blocks
        _advance_blocks(self.sample_rate,
                        len(data) // (self.channel_count * 2))
        return _audiodsp.distortion_s16(
            data,
            min(1.0, max(0.0, _value(self.drive))),
            min(60.0, max(-60.0, _value(self.pre_gain))),
            min(24.0, max(-80.0, _value(self.post_gain))),
            self.mode.value,
            self.soft_clip,
            min(1.0, max(0.0, _value(self.mix))),
        )


class Phaser(_Effect):
    def __init__(self, *, frequency=1000, feedback=None, mix=1, stages=6, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        self.frequency = frequency
        self.feedback = 0.7 if feedback is None else feedback
        self.mix = mix
        self._stages = max(1, int(stages))
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        from array import array
        self._feedback_words = array("h", (0 for _ in range(self.channel_count)))
        self._allpass = array(
            "h", (0 for _ in range(self.channel_count * self._stages)))

    @property
    def stages(self): return self._stages

    @stages.setter
    def stages(self, value):
        from array import array
        self._stages = max(1, int(value))
        self._allpass = array(
            "h", (0 for _ in range(self.channel_count * self._stages)))

    def _reset_state(self):
        self._feedback_words[:] = array("h", [0]) * len(self._feedback_words)
        self._allpass[:] = array("h", [0]) * len(self._allpass)

    @staticmethod
    def _sat16(value, shift=0):
        if value < 0 and shift:
            value += (1 << shift) - 1
        value >>= shift
        return max(-32768, min(32767, value))

    @staticmethod
    def _mixdown(value, voices=2):
        scale = voices
        if value < -28000:
            value = ((value + 28000) * scale >> 16) - 28000
        elif value > 28000:
            value = ((value - 28000) * scale >> 16) + 28000
        return ((value + 32768) & 0xffff) - 32768

    def _process(self, data):
        if not data:
            return data
        from synthio import _advance_blocks
        frame_size = self.channel_count * (self.bits_per_sample // 8)
        _advance_blocks(self.sample_rate, len(data) // frame_size)
        frequency = min(self.sample_rate / 2.0,
                        max(0.0, _value(self.frequency)))
        feedback = int(min(.9, max(.1, _value(self.feedback))) * 32767)
        mix = int(min(1.0, max(0.0, _value(self.mix))) * 32767)
        if mix <= 328:
            return data
        coefficient = int(((1.0 - frequency / (self.sample_rate / 2.0)) /
                           (1.0 + frequency / (self.sample_rate / 2.0))) * 32767)
        if self.bits_per_sample == 16:
            return _audiodsp.phaser_s16(
                data, self._feedback_words, self._allpass,
                self.channel_count, self._stages, frequency,
                self.sample_rate / 2.0,
                min(.9, max(.1, _value(self.feedback))),
                min(1.0, max(0.0, _value(self.mix))),
            )
        # Match the signed-domain conversion used by the C 8-bit path.
        result = bytearray()
        for index, raw in enumerate(data):
            sample = raw if self.samples_signed and raw < 128 else raw - 256
            if not self.samples_signed:
                sample = ((raw ^ 0x80) + 128) % 256 - 128
            channel = index % self.channel_count
            word = self._sat16(sample + self._sat16(
                self._feedback_words[channel] * feedback, 15))
            offset = channel * self._stages
            for stage in range(self._stages):
                state_index = offset + stage
                allpass_word = self._sat16(
                    self._sat16(word * -coefficient, 15) + self._allpass[state_index])
                self._allpass[state_index] = self._sat16(
                    self._sat16(allpass_word * coefficient, 15) + word)
                word = allpass_word
            self._feedback_words[channel] = word
            out = self._mixdown(sample + self._sat16(word * mix, 15)) & 0xff
            result.append(out if self.samples_signed else out ^ 0x80)
        return bytes(result)


# CircuitPython 10.3.0's audiofilters_process_filter_chain: Q15 coefficients
# from the same fast_sincos / Q_rsqrt tick as shared-module/synthio/Biquad.c,
# then synthio_sat16 of the five-product sum. audiodsp_biquad is a different
# arithmetic (wider shift) and is not this helper.
_PAIR_SCALE = 0xfffffff // (32768 * 2 - 28000)


def _sat16(value, shift=0):
    value = int(value)
    if value < 0 and shift:
        value += (1 << shift) - 1
    if shift:
        value >>= shift
    if value > 32767:
        return 32767
    if value < -32768:
        return -32768
    return value


def _mix_down(value, scale=_PAIR_SCALE):
    value = int(value)
    if value < -28000:
        value = (((value + 28000) * scale) >> 16) - 28000
    elif value > 28000:
        value = (((value - 28000) * scale) >> 16) + 28000
    return ((value + 32768) & 0xffff) - 32768


def _q_rsqrt(number):
    bits = struct.unpack("I", struct.pack("f", float(number)))[0]
    bits = (0x5f3759df - (bits >> 1)) & 0xffffffff
    estimate = struct.unpack("f", struct.pack("I", bits))[0]
    return estimate * (1.5 - (float(number) * 0.5 * estimate * estimate))


def _fast_sincos(theta):
    x = (theta * (4.0 / math.pi)) - 1.0
    x2 = x * x
    x3 = x2 * x
    x4 = x2 * x2
    x5 = x2 * x3
    evens = 0.0109 * x4 + -0.21798592 * x2 + 0.70708592
    odds = -0.00171961 * x5 + 0.05707685 * x3 + -0.55535724 * x
    return evens - odds, evens + odds


def _c_round(value):
    return int(math.copysign(math.floor(abs(value) + 0.5), value))


def _cp_biquad_coeffs(biquad, w_scale):
    sine, cosine = _fast_sincos(_value(biquad.frequency) * w_scale)
    quality = _value(biquad.Q)
    mode = biquad.mode.value
    gain = 0.0
    if mode >= 4 and biquad.A is not None:
        gain = _value(biquad.A)
    alpha = sine / (2.0 * quality)
    if mode < 4:
        a0, a1, a2 = 1.0 + alpha, -2.0 * cosine, 1.0 - alpha
        if mode == 0:
            b0 = b2 = (1.0 - cosine) * 0.5
            b1 = 1.0 - cosine
        elif mode == 1:
            b0 = b2 = (1.0 + cosine) * 0.5
            b1 = -(1.0 + cosine)
        elif mode == 2:
            b0, b1, b2 = alpha, 0.0, -alpha
        else:
            b0, b1, b2 = 1.0, -2.0 * cosine, 1.0
    elif mode == 4:
        b0, b1, b2 = 1.0 + alpha * gain, -2.0 * cosine, 1.0 + alpha * gain
        a0, a1, a2 = 1.0 + alpha / gain, -2.0 * cosine, 1.0 - alpha / gain
    else:
        root = gain * _q_rsqrt(gain) if gain else 0.0
        if mode == 5:
            b0 = gain * ((gain + 1) - (gain - 1) * cosine + 2 * root * alpha)
            b1 = 2 * gain * ((gain - 1) - (gain + 1) * cosine)
            b2 = gain * ((gain + 1) - (gain - 1) * cosine - 2 * root * alpha)
            a0 = (gain + 1) + (gain - 1) * cosine + 2 * root * alpha
            a1 = -2 * ((gain - 1) + (gain + 1) * cosine)
            a2 = (gain + 1) + (gain - 1) * cosine - 2 * root * alpha
        else:
            b0 = gain * ((gain + 1) + (gain - 1) * cosine + 2 * root * alpha)
            b1 = -2 * gain * ((gain - 1) + (gain + 1) * cosine)
            b2 = gain * ((gain + 1) + (gain - 1) * cosine - 2 * root * alpha)
            a0 = (gain + 1) - (gain - 1) * cosine + 2 * root * alpha
            a1 = 2 * ((gain - 1) - (gain + 1) * cosine)
            a2 = (gain + 1) - (gain - 1) * cosine - 2 * root * alpha
    reciprocal = 1.0 / a0
    return (
        _c_round(math.ldexp(a1 * reciprocal, 15)),
        _c_round(math.ldexp(a2 * reciprocal, 15)),
        _c_round(math.ldexp(b0 * reciprocal, 15)),
        _c_round(math.ldexp(b1 * reciprocal, 15)),
        _c_round(math.ldexp(b2 * reciprocal, 15)),
    )


def _cp_biquad_sample(coeffs, state, word):
    a1, a2, b0, b1, b2 = coeffs
    x0, x1, y0, y1 = state
    output = _sat16(
        b0 * word + b1 * x0 + b2 * x1 - a1 * y0 - a2 * y1 + (1 << 14), 15)
    state[1], state[0], state[3], state[2] = x0, word, y0, output
    return output


class _FilterChain:
    def __init__(self, filter_in, channel_count):
        from synthio import Biquad
        if filter_in is None:
            items = ()
        elif isinstance(filter_in, (tuple, list)):
            items = tuple(filter_in)
            for item in items:
                if not isinstance(item, Biquad):
                    raise TypeError(
                        "object in filter must be of type Biquad, not %s"
                        % type(item).__name__)
        else:
            if not isinstance(filter_in, Biquad):
                raise TypeError(
                    "filter must be of type Biquad or tuple, not %s"
                    % type(filter_in).__name__)
            items = (filter_in,)
        self.obj = filter_in
        self.objs = items
        self.channel_count = channel_count
        self.states = [[0, 0, 0, 0] for _ in range(len(items) * channel_count)]
        self.coeffs = [(0, 0, 0, 0, 0) for _ in items]

    def __bool__(self):
        return bool(self.objs)

    def reset(self):
        for state in self.states:
            state[:] = [0, 0, 0, 0]

    def tick(self, sample_rate):
        # A multiply by a reciprocal, matching synthio_global_W_scale exactly.
        # `(2 * pi) / sample_rate` differs in the last bits, and Q15 rounding
        # turns that into different coefficients. See audiodsp_biquad_cp_w0().
        w_scale = (2.0 * math.pi) * (1.0 / sample_rate)
        self.coeffs = [_cp_biquad_coeffs(biquad, w_scale)
                       for biquad in self.objs]

    def process(self, channel, word):
        for index, coeffs in enumerate(self.coeffs):
            word = _cp_biquad_sample(
                coeffs, self.states[index * self.channel_count + channel],
                word)
        return word


__all__ = ("Filter", "Distortion", "Phaser", "DistortionMode")
