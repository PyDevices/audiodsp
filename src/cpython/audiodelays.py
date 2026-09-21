"""CircuitPython-compatible streaming delay effects."""

from array import array
import math

import _audiodsp
from audiofilters import _Effect
from audiofilters import _FilterChain
from audiofilters import _mix_down
from audiofilters import _sat16
from audiofilters import _value


class Echo(_Effect):
    _process_silence = True

    def __init__(self, *, max_delay_ms=500, delay_ms=None, decay=None, filter=None, mix=None, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1, freq_shift=True):
        if not 1 <= max_delay_ms <= 4000: raise ValueError("max_delay_ms must be from 1 to 4000")
        self.max_delay_ms, self.delay_ms, self.decay, self.mix = max_delay_ms, 250 if delay_ms is None else delay_ms, 0.7 if decay is None else decay, 0.5 if mix is None else mix
        self.freq_shift = bool(freq_shift)
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        self._maximum_samples = int(self.sample_rate / 1000.0 * self.max_delay_ms)
        self._echo_buffer = array(
            "h", (0 for _ in range(self._maximum_samples * self.channel_count))
        )
        self._left_position = self._right_position = 0
        self.filter = filter

    def _reset_state(self):
        self._echo_buffer[:] = array("h", [0]) * len(self._echo_buffer)
        self._filter_chain.reset()

    @property
    def filter(self):
        return self._filter_chain.obj

    @filter.setter
    def filter(self, value):
        self._filter_chain = _FilterChain(value, self.channel_count)

    def _echo_args(self):
        delay_ms = max(1000.0 / self.sample_rate, _value(self.delay_ms))
        if self.freq_shift:
            delay_samples = self._maximum_samples
            rate = max(int(self.max_delay_ms / delay_ms * 256.0), 1)
        else:
            delay_samples = int(self.sample_rate / 1000.0 * delay_ms)
            delay_samples = min(self._maximum_samples,
                                max(self.buffer_size // 2, delay_samples))
            rate = 1
        return delay_samples, rate, min(1.0, max(0.0, _value(self.decay))), min(
            1.0, max(0.0, _value(self.mix))) * 2.0

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data:
            return data
        from synthio import _advance_blocks
        _advance_blocks(self.sample_rate,
                        len(data) // (self.channel_count * 2))
        delay_samples, rate, decay, mix = self._echo_args()
        if not self._filter_chain:
            result, self._left_position, self._right_position = _audiodsp.echo_s16(
                data, self._echo_buffer, self._left_position, self._right_position,
                delay_samples, self._maximum_samples, rate, decay, mix,
                self.freq_shift, self.channel_count,
            )
            return result
        self._filter_chain.tick(self.sample_rate)
        samples = array("h")
        samples.frombytes(data)
        output = array("h", (0 for _ in range(len(samples))))
        delay = self._echo_buffer
        left, right = self._left_position, self._right_position
        for index, sample in enumerate(samples):
            right_channel = self.channel_count == 2 and index % 2 == 1
            offset = self._maximum_samples if right_channel else 0
            position = right if right_channel else left
            if self.freq_shift:
                echo = delay[(position >> 8) + offset]
                nxt = position + rate
                for tap in range(position >> 8, nxt >> 8):
                    word = int(delay[(tap % delay_samples) + offset] * decay + sample)
                    delay[(tap % delay_samples) + offset] = self._filter_chain.process(
                        1 if right_channel else 0, _mix_down(word))
                position = nxt % (delay_samples << 8)
            else:
                echo = delay[position + offset]
                word = int(echo * decay + sample)
                delay[position + offset] = self._filter_chain.process(
                    1 if right_channel else 0, _mix_down(word))
                position += 1
                if position >= delay_samples:
                    position = 0
            word = int(sample * ((2.0 - mix) if (2.0 - mix) < 1.0 else 1.0)
                       + echo * (mix if mix < 1.0 else 1.0))
            output[index] = _mix_down(word)
            if right_channel:
                right = position
            else:
                left = position
        self._left_position, self._right_position = left, right
        return output.tobytes()


class Chorus(_Effect):
    def __init__(self, *, max_delay_ms=50, delay_ms=None, voices=None, mix=None, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        if not 1 <= max_delay_ms <= 4000: raise ValueError("max_delay_ms must be from 1 to 4000")
        self.max_delay_ms, self.delay_ms = max_delay_ms, 50 if delay_ms is None else delay_ms
        self.voices, self.mix = 1.0 if voices is None else voices, 0.5 if mix is None else mix
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        self._maximum_samples = int(
            self.sample_rate / 1000.0 * self.max_delay_ms * self.channel_count)
        self._chorus_buffer = array(
            "h", (0 for _ in range(self._maximum_samples)))
        self._position = 0

    def _reset_state(self):
        self._chorus_buffer[:] = array("h", [0]) * len(self._chorus_buffer)

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data:
            return data
        from synthio import _advance_blocks
        _advance_blocks(self.sample_rate,
                        len(data) // (self.channel_count * 2))
        voices = max(1, int(_value(self.voices)))
        mix = min(1.0, max(0.0, _value(self.mix)))
        delay_ms = max(1000.0 / self.sample_rate, _value(self.delay_ms))
        delay_samples = int(self.sample_rate / 1000.0 * delay_ms)
        delay_samples *= self.channel_count
        delay_samples = min(self._maximum_samples, max(1, delay_samples))
        result, self._position = _audiodsp.chorus_s16(
            data, self._chorus_buffer, self._position, delay_samples,
            self._maximum_samples, voices, mix,
        )
        return result


class MultiTapDelay(_Effect):
    _process_silence = True

    def __init__(self, *, max_delay_ms=500, delay_ms=250, decay=None, mix=None, taps=None, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        if not 1 <= max_delay_ms <= 4000: raise ValueError("max_delay_ms must be from 1 to 4000")
        self.max_delay_ms, self.delay_ms = max_delay_ms, delay_ms
        self.decay, self.mix = 0.7 if decay is None else decay, 0.25 if mix is None else mix
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        self._maximum_samples = int(
            self.sample_rate / 1000.0 * self.max_delay_ms)
        self._delay_buffer = array("h", (0 for _ in range(
            self._maximum_samples * self.channel_count)))
        self._position = 0
        self.taps = taps

    def _reset_state(self):
        self._delay_buffer[:] = array("h", [0]) * len(self._delay_buffer)

    @property
    def taps(self):
        return None if not self._tap_positions else tuple(zip(
            self._tap_positions, self._tap_levels))

    @taps.setter
    def taps(self, value):
        if value is None:
            items = ()
        else:
            items = tuple(value)
            if not items: raise ValueError("items length must be at least 1")
        positions, levels = [], []
        for item in items:
            if isinstance(item, (tuple, list)):
                if len(item) != 2: raise ValueError("items length must be 2")
                position, level = item
            else:
                position, level = item, 1.0
            position, level = float(position), float(level)
            if not 0 <= position <= 1: raise ValueError("position must be from 0 to 1")
            if not 0 <= level <= 1: raise ValueError("level must be from 0 to 1")
            positions.append(position); levels.append(level)
        self._tap_positions, self._tap_levels = tuple(positions), tuple(levels)

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data:
            return data
        from synthio import _advance_blocks
        _advance_blocks(self.sample_rate,
                        len(data) // (self.channel_count * 2))
        delay_ms = max(1000.0 / self.sample_rate, float(self.delay_ms))
        delay_samples = int(self.sample_rate / 1000.0 * delay_ms)
        minimum_samples = self.buffer_size // (self.channel_count * 2)
        delay_samples = min(self._maximum_samples,
                            max(minimum_samples, delay_samples))
        offsets = array("I", (int(delay_samples * position)
                              for position in self._tap_positions))
        levels = array("d", self._tap_levels)
        result, self._position = _audiodsp.multitap_s16(
            data, self._delay_buffer, self._position, delay_samples,
            self.channel_count, offsets, levels,
            min(1.0, max(0.0, _value(self.decay))),
            min(1.0, max(0.0, _value(self.mix))) * 2.0,
        )
        return result


class PitchShift(_Effect):
    def __init__(self, *, semitones=0, mix=1, window=1024, overlap=128, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        self.semitones, self.mix, self.window, self.overlap = semitones, mix, int(window), int(overlap)
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        self._window_samples = self.window // 2 // self.channel_count
        self._overlap_samples = self.overlap // 2 // self.channel_count
        self._window_buffer = array("h", (0 for _ in range(
            self._window_samples * self.channel_count)))
        self._overlap_buffer = array("h", (0 for _ in range(
            self._overlap_samples * self.channel_count)))
        self._window_index = self._overlap_index = self._read_index = 0

    def _reset_state(self):
        self._window_buffer[:] = array("h", [0]) * len(self._window_buffer)
        self._overlap_buffer[:] = array("h", [0]) * len(self._overlap_buffer)

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data:
            return data
        from synthio import _advance_blocks
        _advance_blocks(self.sample_rate,
                        len(data) // (self.channel_count * 2))
        read_rate = int(2.0 ** (_value(self.semitones) / 12.0) * 256)
        result, self._window_index, self._overlap_index, self._read_index = (
            _audiodsp.pitchshift_s16(
                data, self._window_buffer, self._overlap_buffer,
                self._window_samples, self._overlap_samples,
                self.channel_count, read_rate, self._window_index,
                self._overlap_index, self._read_index,
                min(1.0, max(0.0, _value(self.mix))) * 2.0,
            )
        )
        return result


class Flanger(_Effect):
    def __init__(self, *, max_delay_ms=10, min_delay_ms=None, rate=None, depth=None, feedback=None, mix=None, invert=False, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        if not 1 <= max_delay_ms <= 100: raise ValueError("max_delay_ms must be from 1 to 100")
        self.max_delay_ms = max_delay_ms
        self.min_delay_ms = 1.0 if min_delay_ms is None else min_delay_ms
        self.rate, self.depth = 0.5 if rate is None else rate, 0.5 if depth is None else depth
        self.feedback, self.mix = 0.5 if feedback is None else feedback, 0.5 if mix is None else mix
        self.invert = bool(invert)
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        self._delay_buffer_frames = int(self.sample_rate / 1000.0 * max_delay_ms) + 2
        self._delay_buffer = array(
            "h", (0 for _ in range(self._delay_buffer_frames * self.channel_count)))
        self._write_pos = [0, 0]
        self._lfo_phase = [0, 0]

    def _reset_state(self):
        self._delay_buffer[:] = array("h", [0]) * len(self._delay_buffer)
        self._write_pos = [0, 0]
        self._lfo_phase = [0, 0]

    @staticmethod
    def _ms_to_frames_q16(ms, sample_rate, frames):
        count = ms * sample_rate / 1000.0
        count = min(max(count, 1.0), min(frames - 2, 65535))
        return int(count * 65536.0) & 0xffffffff

    @staticmethod
    def _triangle(phase):
        tri = (phase & 0xffffffff) >> 15
        if tri > 65535:
            tri = 131071 - tri
        return tri

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data:
            return data
        from synthio import _advance_blocks
        samples = array("h")
        samples.frombytes(data)
        _advance_blocks(self.sample_rate, len(samples) // self.channel_count)
        sample_ms = 1000.0 / self.sample_rate
        min_delay = min(self.max_delay_ms, max(sample_ms, _value(self.min_delay_ms)))
        rate = min(20.0, max(0.0, _value(self.rate)))
        depth = min(1.0, max(0.0, _value(self.depth)))
        feedback = int(min(0.95, max(-0.95, _value(self.feedback))) * 32767)
        mix = int(min(1.0, max(0.0, _value(self.mix))) * 32767)
        sweep_top = min_delay + depth * (self.max_delay_ms - min_delay)
        delay_min = self._ms_to_frames_q16(
            min_delay, self.sample_rate, self._delay_buffer_frames)
        delay_span = (self._ms_to_frames_q16(
            sweep_top, self.sample_rate, self._delay_buffer_frames) - delay_min) & 0xffffffff
        phase_inc = int(min(rate / self.sample_rate, 0.5) * 4294967296.0) & 0xffffffff
        frames = self._delay_buffer_frames
        line = self._delay_buffer
        output = array("h", (0 for _ in range(len(samples))))
        for index, sample in enumerate(samples):
            channel = 1 if (self.channel_count == 2 and index % 2 == 1) else 0
            self._lfo_phase[channel] = (self._lfo_phase[channel] + phase_inc) & 0xffffffff
            tri = self._triangle(self._lfo_phase[channel])
            delay_q16 = (delay_min + ((delay_span * tri) >> 16)) & 0xffffffff
            delay_int = delay_q16 >> 16
            delay_frac = delay_q16 & 0xffff
            plane = channel * frames
            write = self._write_pos[channel]
            r0 = write + frames - delay_int
            if r0 >= frames:
                r0 -= frames
            r1 = frames - 1 if r0 == 0 else r0 - 1
            s0 = line[plane + r0]
            s1 = line[plane + r1]
            wet = s0 + (((s1 - s0) * delay_frac) >> 16)
            line[plane + write] = _sat16(sample + _sat16(wet * feedback, 15), 0)
            self._write_pos[channel] = 0 if write + 1 == frames else write + 1
            if mix <= 328:
                word = sample
            else:
                wet_word = _sat16(wet * mix, 15)
                word = sample - wet_word if self.invert else sample + wet_word
                word = _mix_down(word)
            output[index] = word
        return output.tobytes()


class GranularPitchShift(_Effect):
    _MAX_GRAINS = 8

    def __init__(self, *, semitones=0, mix=1, grain_size=1024, density=2, spread=0.0, buffer_size=512, sample_rate=8000, bits_per_sample=16, samples_signed=True, channel_count=1):
        if grain_size < 2: raise ValueError("grain_size must be at least 2")
        if not 1 <= density <= self._MAX_GRAINS: raise ValueError("density must be from 1 to 8")
        self.semitones, self.mix = semitones, mix
        self.grain_size, self.density = int(grain_size), int(density)
        self.spread = min(1.0, max(0.0, float(spread)))
        self._init_format(buffer_size=buffer_size, sample_rate=sample_rate, bits_per_sample=bits_per_sample, samples_signed=samples_signed, channel_count=channel_count)
        self._capture_len = self.grain_size * 2
        self._capture = array(
            "h", (0 for _ in range(self._capture_len * self.channel_count)))
        self._write_index = 0
        self._rng_state = 0x1234abcd
        self._grain_gain = min(1 << 15, (1 << 15) * 2 // self.density)
        denom = (self.grain_size - 1) if self.grain_size > 1 else 1.0
        self._envelope = array("h", (
            int(0.5 * (1.0 - math.cos(2.0 * math.pi * n / denom)) * 32767.0)
            for n in range(self.grain_size)))
        self._grains = [self._empty_grain() for _ in range(self._MAX_GRAINS)]
        self._until_next = 0
        self._read_rate = int(2.0 ** (_value(self.semitones) / 12.0) * 256)
        self._current_semitones = _value(self.semitones)

    @staticmethod
    def _empty_grain():
        return {"active": False, "read_index": 0, "read_rate": 0,
                "phase": 0, "length": 0}

    def _reset_state(self):
        self._capture[:] = array("h", [0]) * len(self._capture)
        self._grains = [self._empty_grain() for _ in range(self._MAX_GRAINS)]
        self._until_next = 0
        self._write_index = 0

    def _rand(self):
        x = self._rng_state & 0xffffffff
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17
        x ^= (x << 5) & 0xffffffff
        self._rng_state = x & 0xffffffff
        return self._rng_state

    def _launch(self):
        for grain in self._grains:
            if grain["active"]:
                continue
            jitter = 0
            if self.spread > 0.0:
                max_jitter = int(self.spread * self.grain_size)
                if max_jitter > 0:
                    jitter = self._rand() % (max_jitter + 1)
            start = (self._write_index + self._capture_len - self.grain_size - jitter) % self._capture_len
            grain["active"] = True
            grain["read_index"] = start << 8
            grain["read_rate"] = self._read_rate
            grain["phase"] = 0
            grain["length"] = self.grain_size
            return

    def _process(self, data):
        if self.bits_per_sample != 16 or not self.samples_signed or not data:
            return data
        from synthio import _advance_blocks
        samples = array("h")
        samples.frombytes(data)
        _advance_blocks(self.sample_rate, len(samples) // self.channel_count)
        semitones = _value(self.semitones)
        mix = min(1.0, max(0.0, _value(self.mix))) * 2.0
        if semitones != self._current_semitones:
            self._read_rate = int(2.0 ** (semitones / 12.0) * 256)
            self._current_semitones = semitones
        spacing = max(1, self.grain_size // self.density)
        output = array("h", (0 for _ in range(len(samples))))
        capture = self._capture
        length = self._capture_len
        for index, sample in enumerate(samples):
            buf_offset = 1 if (self.channel_count == 2 and index % 2 == 1) else 0
            capture[self._write_index + length * buf_offset] = sample
            word = 0
            for grain in self._grains:
                if not grain["active"]:
                    continue
                ipart = grain["read_index"] >> 8
                frac = grain["read_index"] & 255
                index_lo = ipart % length
                index_hi = (ipart + 1) % length
                sample_lo = capture[index_lo + length * buf_offset]
                sample_hi = capture[index_hi + length * buf_offset]
                grain_out = sample_lo + (((sample_hi - sample_lo) * frac) >> 8)
                word += (grain_out * self._envelope[grain["phase"]]) >> 15
            word = int((word * self._grain_gain) >> 15)
            dry = (2.0 - mix) if (2.0 - mix) < 1.0 else 1.0
            wet = mix if mix < 1.0 else 1.0
            word = int(sample * dry + word * wet)
            output[index] = _mix_down(word)
            if self.channel_count == 1 or buf_offset:
                for grain in self._grains:
                    if not grain["active"]:
                        continue
                    grain["read_index"] = (grain["read_index"] + grain["read_rate"]) & 0xffffffff
                    grain["phase"] += 1
                    if grain["phase"] >= grain["length"]:
                        grain["active"] = False
                if self._until_next == 0:
                    self._launch()
                    self._until_next = spacing
                else:
                    self._until_next -= 1
                self._write_index += 1
                if self._write_index >= length:
                    self._write_index = 0
        return output.tobytes()


__all__ = ("Echo", "Chorus", "PitchShift", "MultiTapDelay", "Flanger",
           "GranularPitchShift")
