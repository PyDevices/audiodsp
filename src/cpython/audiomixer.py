"""CircuitPython-compatible PCM mixer."""

from struct import pack as _pack, unpack as _unpack

from audiocore import GET_BUFFER_MORE_DATA, _AudioSample, get_buffer, reset_buffer
import _audioif


def _f32(value):
    """`value` rounded to `float`, the way a C `(float)` cast rounds it.

    `struct` is the standard library and this wheel has no dependencies, so
    this is not numpy: `audiomixer` is a core module that has to import on a
    bare install. See `_mod_mul` for why it is here at all.
    """
    return _unpack("f", _pack("f", value))[0]


def _mod_mul(level):
    """A voice's Q15 level as the single-precision multiplier the native
    kernel uses.

    `src/audiomixer/Mixer.c`'s `mult16signed` computes

        float mod_mul = (float)level / (float)((1 << 15) - 1);
        int32_t intermediate = (int32_t)(ai * mod_mul);

    -- the quotient and the product both in `float`, then truncation toward
    zero. This twin used to form the same product in `float64` and truncate
    that, which lands on a different integer whenever the true product sits
    within a float32 rounding of one: 56 of the 65536 `int16` values at level
    100/127 alone, each off by a whole LSB. Inaudible at -90 dBFS, but the
    effects program's class gate compares CPython bytes with MicroPython bytes,
    so it read as nondeterminism. audioif#84.

    Both roundings here are single, not double: `level` and 32767 are exact in
    `float`, and a `float` divide computed in `double` and rounded once is the
    correctly rounded `float` quotient (53 >= 2*24 + 2). The product of two
    `float`s is exact in `double` for the same reason, so rounding it once is
    the `float` product itself.
    """
    return _f32(_f32(level) / _f32(32767.0))


def _source_chunk(sample):
    result, data = get_buffer(sample)
    raw = bytes(data)
    # CircuitPython's mixer consumes packed 32-bit words. Any trailing bytes
    # that do not form a word are therefore intentionally ignored.
    return result, raw[:len(raw) // 4 * 4]


class MixerVoice:
    def __init__(self, mixer):
        self._mixer = mixer
        self._sample = None
        self._loop = False
        self.level = 1.0
        self.panning = 0.0
        self._remaining = b""
        self._source_more = False
        # The level this voice last actually rendered at, per channel. Zero on
        # a fresh voice, as CircuitPython's zero-initialised
        # active_lo_level/active_hi_level are, and never cleared thereafter.
        self._active_level = (0, 0)

    @property
    def playing(self): return self._sample is not None

    def play(self, sample, *, loop=False):
        self._mixer._check_sample(sample)
        reset_buffer(sample)
        self._sample, self._loop = sample, bool(loop)
        result, self._remaining = _source_chunk(sample)
        self._source_more = result == GET_BUFFER_MORE_DATA
        # A LOOPING SOURCE THAT CANNOT FILL ONE PACKED WORD NEVER ENDS. The
        # mix-down consumes whole 32-bit words (`_source_chunk` drops the
        # trailing bytes that do not form one), so a two-byte sample offers
        # nothing to take, and looping means it never reaches the exit that
        # stops a finished voice. On the native builds that is a hang rather
        # than a short render: audioif#85.
        #
        # Read off the fetch just made rather than off a declared length,
        # because a source here is any object with `_get_buffer` and has no
        # `max_buffer_length` to read. `src/audiomixer/MixerVoice.c` applies
        # the same rule at the same moment, and carries why this refuses
        # rather than pads.
        #
        # `voice.loop = True` set AFTER play() is deliberately not guarded;
        # the empty-fetch counter in `_get_buffer` covers it by stopping the
        # voice, as it has since audioif#24.
        if self._loop and not self._remaining and not self._source_more:
            self._sample, self._loop = None, False
            raise ValueError(
                "A looped sample must fill at least one 32-bit word")

    @property
    def loop(self):
        return self._loop

    @loop.setter
    def loop(self, value):
        # The native binding has carried `MixerVoice.loop` as a GETSET since
        # the tier-3 port and CircuitPython has it too; this side only ever had
        # the `play(loop=)` argument, so the property is added here rather than
        # being a new surface. It is deliberately unguarded -- see play().
        self._loop = bool(value)

    def stop(self): self._sample = None

    def reset(self):
        """Rewind to the start of whatever is playing, keeping it playing."""
        if self._sample is None:
            return
        reset_buffer(self._sample)
        result, self._remaining = _source_chunk(self._sample)
        self._source_more = result == GET_BUFFER_MORE_DATA


class Mixer(_AudioSample):
    def __init__(self, *, voice_count=2, buffer_size=1024, channel_count=2,
                 bits_per_sample=16, samples_signed=True, sample_rate=8000):
        if not 1 <= voice_count <= 255: raise ValueError("voice_count must be from 1 to 255")
        if channel_count not in (1, 2): raise ValueError("channel_count must be 1 or 2")
        if bits_per_sample not in (8, 16): raise ValueError("bits_per_sample must be 8 or 16")
        if sample_rate < 1: raise ValueError("sample_rate must be at least 1")
        self.sample_rate, self.channel_count = int(sample_rate), int(channel_count)
        self.bits_per_sample, self.samples_signed = int(bits_per_sample), bool(samples_signed)
        self.buffer_size = int(buffer_size)
        self._render_size = self.buffer_size // 2 // 4 * 4
        self.voice = tuple(MixerVoice(self) for _ in range(voice_count))
        self._deinited = False

    @property
    def playing(self): return any(voice.playing for voice in self.voice)

    def _release(self):
        for voice in self.voice:
            voice.stop()
            voice._remaining = b""

    def _check_sample(self, sample):
        self._check()
        from audiospeed import Resampler
        if isinstance(sample, Resampler):
            for name in ("channel_count", "bits_per_sample", "samples_signed"):
                if getattr(sample, name) != getattr(self, name):
                    raise ValueError("The sample's %s does not match" % name)
            sample._bind_sample_rate(self.sample_rate)
            return
        for name in ("sample_rate", "channel_count", "bits_per_sample", "samples_signed"):
            if getattr(sample, name) != getattr(self, name):
                raise ValueError("The sample's %s does not match" % name)

    def play(self, sample, *, voice=0, loop=False): self.voice[voice].play(sample, loop=loop)
    def stop_voice(self, voice=0): self.voice[voice].stop()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        # Deviation from upstream, which stops every voice here instead of
        # rewinding them -- see docs/upstream-diff.md, "Resetting a Mixer
        # silenced it".
        self._check()
        for voice in self.voice:
            voice.reset()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        chunks = []
        active = [voice for voice in self.voice if voice.playing]
        if not active:
            if not self.samples_signed and self.bits_per_sample == 16:
                silence = b"\x00\x80" * (self._render_size // 2)
            else:
                neutral = 128 if not self.samples_signed else 0
                silence = bytes([neutral]) * self._render_size
            return GET_BUFFER_MORE_DATA, memoryview(silence)
        for voice in active:
            output = bytearray()
            # A source may claim GET_BUFFER_MORE_DATA and hand back nothing.
            # That takes 0 bytes, grows `output` not at all, and satisfies
            # neither exit below - so the loop spins forever and the whole
            # render hangs rather than failing. Seen for real: a probe whose
            # effect returned a zero-length buffer while still reporting
            # MORE_DATA made verify_acceptance run until it was killed at 45 s
            # with no output, where verify_streaming failed in 0.07 s. In CI
            # that is an untimed job hang, not a named failure.
            #
            # One empty chunk is tolerated, because a source is allowed a beat
            # to produce; a second consecutive one means it is not going to.
            # The voice stops, its remainder is zero-filled like any other
            # short voice, and the mix continues. See issue #24.
            empty_fetches = 0
            while len(output) < self._render_size and voice._sample is not None:
                take = min(self._render_size - len(output), len(voice._remaining))
                output += voice._remaining[:take]
                voice._remaining = voice._remaining[take:]
                if len(output) == self._render_size:
                    break
                if not voice._remaining:
                    if not voice._source_more:
                        if voice._loop:
                            reset_buffer(voice._sample, single_channel_output, audio_channel)
                        else:
                            voice.stop()
                            break
                    if voice._sample is not None:
                        result, voice._remaining = _source_chunk(voice._sample)
                        voice._source_more = result == GET_BUFFER_MORE_DATA
                        if not voice._remaining and not voice._source_more:
                            voice.stop()
                            break
                        if not voice._remaining:
                            empty_fetches += 1
                            if empty_fetches > 1:
                                voice.stop()
                                break
                        else:
                            empty_fetches = 0
            data = bytes(output) + bytes(self._render_size - len(output))
            if self.bits_per_sample == 16:
                import array
                samples = array.array("h"); samples.frombytes(data)
                from audiofilters import _value
                level = int(min(1.0, max(0.0, _value(voice.level))) * 32768)
                panning_value = min(32767 / 32768,
                                    max(-32767 / 32768,
                                        _value(voice.panning)))
                panning = int(panning_value * 32768 +
                              (0.5 if panning_value >= 0 else -0.5))
                left = right = level
                if self.channel_count == 2:
                    # panning > 0 attenuates the LEFT channel. CircuitPython
                    # 10.3.0 flipped this and matched synthio.Note to it.
                    left_scale = 32767 - panning if panning >= 0 else 32768
                    right_scale = 32768 if panning >= 0 else 32767 + panning
                    left = (left_scale * level) >> 15
                    right = (right_scale * level) >> 15
                pending = (left, right)
                # A level or pan move waits for a zero crossing so it cannot
                # click. The samples are read two at a time because that is the
                # packed 32-bit word the gate tests; see
                # audioif_assign_packed_level in src/shared/audioif_synth_dsp.c,
                # which is the authority this mirrors.
                active = list(voice._active_level)
                mul_lo, mul_hi = _mod_mul(active[0]), _mod_mul(active[1])
                last_lo = last_hi = 0
                scaled = []
                for index in range(0, len(samples), 2):
                    lo = samples[index]
                    hi = samples[index + 1] if index + 1 < len(samples) else 0
                    if active[0] != pending[0] or active[1] != pending[1]:
                        if (lo == 0 or hi == 0
                                or ((last_lo or last_hi)
                                    and ((last_lo < 0) != (lo < 0)
                                         or (last_hi < 0) != (hi < 0)))):
                            active[0], active[1] = pending
                            mul_lo = _mod_mul(active[0])
                            mul_hi = _mod_mul(active[1])
                        else:
                            last_lo, last_hi = lo, hi
                    scaled.append(max(-32768, min(32767,
                        int(_f32(lo * mul_lo)))))
                    if index + 1 < len(samples):
                        scaled.append(max(-32768, min(32767,
                            int(_f32(hi * mul_hi)))))
                # Forced at the block boundary: at most one block of delay.
                voice._active_level = pending
                data = array.array("h", scaled).tobytes()
            chunks.append(data)
        size = max(map(len, chunks), default=0)
        chunks = [chunk + bytes(size - len(chunk)) for chunk in chunks]
        if self.bits_per_sample == 16 and chunks:
            mixed = chunks[0]
            for chunk in chunks[1:]: mixed = _audioif.mix_s16(mixed, chunk)
        else:
            neutral = 128 if not self.samples_signed else 0
            mixed = bytes(max(0, min(255, sum(chunk[i] - neutral for chunk in chunks) + neutral)) for i in range(size))
        return GET_BUFFER_MORE_DATA, memoryview(mixed)


__all__ = ("Mixer",)
