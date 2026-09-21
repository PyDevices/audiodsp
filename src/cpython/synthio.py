"""CircuitPython-compatible synthesizer API for CPython."""

from array import array
from collections import namedtuple
from enum import Enum
import math

import _audiodsp
from audiocore import GET_BUFFER_MORE_DATA, _AudioSample

waveform_max_length = 16384
_DEFAULT_WAVEFORM = array("h", (-32768, 32767))
_MIDI_BASE_FREQUENCIES = (8372, 8870, 9397, 9956, 10548, 11175, 11840,
                          12544, 13290, 14080, 14917, 15804)
_DEFAULT_LFO_WAVEFORM = array("h", (0, 32767, 0, -32767))
_BLOCK_TICK = 0
_BLOCK_SECONDS = 0.0


class _CircuitEnum(Enum):
    def __repr__(self):
        return "{}.{}.{}".format(
            self.__class__.__module__, self.__class__.__name__, self.name)

    __str__ = __repr__


class FilterMode(_CircuitEnum):
    LOW_PASS = 0
    HIGH_PASS = 1
    BAND_PASS = 2
    NOTCH = 3
    PEAKING_EQ = 4
    LOW_SHELF = 5
    HIGH_SHELF = 6


class MathOperation(_CircuitEnum):
    SUM = 0
    ADD_SUB = 1
    PRODUCT = 2
    MUL_DIV = 3
    SCALE_OFFSET = 4
    OFFSET_SCALE = 5
    LERP = 6
    CONSTRAINED_LERP = 7
    DIV_ADD = 8
    ADD_DIV = 9
    MID = 10
    MIN = 11
    MAX = 12
    ABS = 13

    def __call__(self, a, b=0, c=1):
        return Math(self, a, b, c)


class EnvelopeState(_CircuitEnum):
    ATTACK = 0
    DECAY = 1
    SUSTAIN = 2
    RELEASE = 3


_Envelope = namedtuple("Envelope", "attack_time decay_time release_time attack_level sustain_level")


class Envelope(_Envelope):
    __slots__ = ()

    def __new__(cls, *, attack_time=0.1, decay_time=0.05, release_time=0.2, attack_level=1.0, sustain_level=0.8):
        values = (attack_time, decay_time, release_time, attack_level, sustain_level)
        if any(float(v) < 0 for v in values[:3]):
            raise ValueError("envelope times must be non-negative")
        if any(not 0 <= float(v) <= 1 for v in values[3:]):
            raise ValueError("envelope levels must be between 0 and 1")
        return super().__new__(cls, *(float(v) for v in values))


def _value(value):
    if value is None:
        return 0.0
    evaluator = getattr(value, "_evaluate", None)
    if evaluator is not None:
        return float(evaluator(_BLOCK_TICK))
    return float(value.value if hasattr(value, "value") else value)


def _advance_blocks(sample_rate, sample_count=256):
    global _BLOCK_TICK, _BLOCK_SECONDS
    _BLOCK_TICK += 1
    _BLOCK_SECONDS = sample_count / sample_rate


class Math:
    def __init__(self, operation, a, b=0, c=1):
        if not isinstance(operation, MathOperation):
            raise TypeError("operation must be a MathOperation")
        self.operation = operation
        self.a, self.b, self.c = a, b, c
        self._last_tick = -1
        self._cached_value = 0.0

    @staticmethod
    def _input(value):
        return float(value) if isinstance(value, (int, float)) else value

    @property
    def a(self): return self._a
    @a.setter
    def a(self, value): self._a = self._input(value)
    @property
    def b(self): return self._b
    @b.setter
    def b(self, value): self._b = self._input(value)
    @property
    def c(self): return self._c
    @c.setter
    def c(self, value): self._c = self._input(value)

    @property
    def value(self):
        return self._cached_value

    def _evaluate(self, tick):
        if self._last_tick == tick:
            return self._cached_value
        a, b, c = _value(self.a), _value(self.b), _value(self.c)
        op = self.operation
        if op is MathOperation.SUM: result = a + b + c
        elif op is MathOperation.ADD_SUB: result = a + b - c
        elif op is MathOperation.PRODUCT: result = a * b * c
        elif op is MathOperation.MUL_DIV: result = a * b / c if c else 0.0
        elif op is MathOperation.SCALE_OFFSET: result = a * b + c
        elif op is MathOperation.OFFSET_SCALE: result = (a + b) * c
        if op in (MathOperation.LERP, MathOperation.CONSTRAINED_LERP):
            amount = min(1.0, max(0.0, c)) if op is MathOperation.CONSTRAINED_LERP else c
            result = a * (1.0 - amount) + b * amount
        elif op is MathOperation.DIV_ADD: result = a / b + c if b else 0.0
        elif op is MathOperation.ADD_DIV: result = (a + b) / c if c else 0.0
        elif op is MathOperation.MID: result = sorted((a, b, c))[1]
        elif op is MathOperation.MIN: result = min(a, b, c)
        elif op is MathOperation.MAX: result = max(a, b, c)
        elif op is MathOperation.ABS: result = abs(a)
        self._last_tick, self._cached_value = tick, result
        return result


class LFO:
    def __init__(self, waveform=None, *, rate=1, scale=1, offset=0, phase_offset=0, once=False, interpolate=True):
        global _BLOCK_SECONDS
        self.waveform = waveform
        self.rate, self.scale, self.offset = rate, scale, offset
        self.phase_offset = phase_offset
        self.once, self.interpolate = bool(once), bool(interpolate)
        self._phase = 0.0
        self._last_tick = -1
        self._cached_value = 0.0
        _BLOCK_SECONDS = 0.0
        self._evaluate(_BLOCK_TICK)

    @property
    def value(self):
        return self._cached_value

    @property
    def phase(self):
        return self._phase

    def _evaluate(self, tick):
        if self._last_tick == tick:
            return self._cached_value
        rate = _value(self.rate) * _BLOCK_SECONDS
        phase_offset = _value(self.phase_offset)
        phase = self._phase + rate + phase_offset
        if self.once:
            phase = min(32767 / 32768, max(0.0, phase))
        else:
            phase -= math.floor(phase)
        self._phase = phase - phase_offset
        samples = _DEFAULT_LFO_WAVEFORM if self.waveform is None else self.waveform
        position = phase * (len(samples) - int(self.once))
        index = int(math.floor(position))
        raw = float(samples[index])
        if self.interpolate:
            following_index = index + 1
            if following_index == len(samples):
                following_index = index if self.once else 0
            fraction = position - index
            raw = raw * (1.0 - fraction) + float(samples[following_index]) * fraction
        result = math.ldexp(raw, -15) * _value(self.scale) + _value(self.offset)
        self._last_tick, self._cached_value = tick, result
        return result

    def _tick(self, seconds=1 / 48000):
        global _BLOCK_TICK, _BLOCK_SECONDS
        _BLOCK_TICK += 1
        _BLOCK_SECONDS = seconds
        return self._evaluate(_BLOCK_TICK)

    def retrigger(self):
        self._phase = 0.0


def lfo_tick(*blocks):
    _advance_blocks(48000, 256)
    return tuple(_value(block) for block in blocks)


class Biquad:
    def __init__(self, mode, frequency, Q=0.7071067811865475, A=None):
        if not isinstance(mode, FilterMode):
            raise TypeError("mode must be a FilterMode")
        self.mode, self.frequency, self.Q, self.A = mode, frequency, Q, A


class Note:
    #: The synthesizer channel this note was admitted on, assigned by press().
    #: CircuitPython's channel index, which the per-channel zero-crossing gate
    #: state is keyed to. A note that has never been pressed has no channel; 0
    #: is the value a stray render would read, and is deliberate rather than an
    #: error, matching the note_obj[0] a never-admitted note would occupy.
    _slot = 0

    def __init__(self, frequency, *, panning=0, amplitude=1, bend=0, waveform=None,
                 waveform_loop_start=0, waveform_loop_end=waveform_max_length,
                 envelope=None, filter=None, ring_frequency=0, ring_bend=0,
                 ring_waveform=None, ring_waveform_loop_start=0,
                 ring_waveform_loop_end=waveform_max_length):
        self.frequency = float(frequency)
        self.panning, self.amplitude, self.bend = panning, amplitude, bend
        self.waveform = waveform
        self.waveform_loop_start, self.waveform_loop_end = waveform_loop_start, waveform_loop_end
        self.envelope, self.filter = envelope, filter
        self.ring_frequency, self.ring_bend = float(ring_frequency), ring_bend
        self.ring_waveform = ring_waveform
        self.ring_waveform_loop_start = ring_waveform_loop_start
        self.ring_waveform_loop_end = ring_waveform_loop_end
        self._accum = 0
        # The usermod keeps this per channel and, unlike `accum`, never
        # resets it on a press - so it is deliberately not reset here either.
        self._ring_accum = 0
        self._midi_note = None
        self._envelope_state = None
        self._envelope_seen = None
        self._released = False
        # One state per possible cascade stage (audiodsp extension #11:
        # ``filter`` accepts a Biquad or a tuple/list of up to four
        # Biquads applied in series). A single filter uses stage 0 and
        # behaves exactly as stock CircuitPython.
        self._filter_states = tuple(_audiodsp.BiquadState() for _ in range(4))

    @property
    def filter(self):
        return self._filter

    @filter.setter
    def filter(self, value):
        if value is not None and not isinstance(value, Biquad):
            if isinstance(value, (tuple, list)):
                if len(value) > 4:
                    raise ValueError("filter cascade too long")
                for stage in value:
                    if not isinstance(stage, Biquad):
                        raise TypeError("filter must be of type Biquad, not %s"
                                        % type(stage).__name__)
            else:
                raise TypeError("filter must be of type Biquad, not %s"
                                % type(value).__name__)
        self._filter = value

    def _reset_filter(self):
        for state in self._filter_states:
            state.reset()


class Synthesizer(_AudioSample):
    max_polyphony = 64

    def __init__(self, *, sample_rate=11025, channel_count=1, waveform=None, envelope=None):
        if channel_count not in (1, 2): raise ValueError("channel_count must be 1 or 2")
        if sample_rate < 1: raise ValueError("sample_rate must be at least 1")
        self.sample_rate, self.channel_count = int(sample_rate), int(channel_count)
        self.bits_per_sample, self.samples_signed = 16, True
        self.waveform, self.envelope = waveform, envelope
        self.blocks = []
        self._notes = []
        # The loudness each voice slot last actually rendered at. CircuitPython
        # keys this to the channel (`synth->active_loudness[chan]`), not to the
        # note, and never clears it -- not on reset_buffer, not when a slot
        # changes hands. Zero-initialised there, so a slot's first block is
        # gated until the oscillator crosses zero.
        #
        # `_notes` is a list here rather than CircuitPython's fixed
        # `note_obj[chan]` array, so each note carries the channel index it was
        # admitted on (`note._slot`) and this is indexed by that -- NOT by the
        # note's position in the list. The list compacts when a note finishes
        # and CircuitPython's channels do not, so using the list position made
        # a note read another note's gate state after any voice ended. It cost
        # one block of `synthtools_acceptance`'s `bend` and one of its release
        # tail, and nothing else: the gate is forced to pending at every block
        # boundary, so a wrong read cannot propagate past the block it happens
        # in, which is what made it look like rounding rather than aliasing.
        self._active_loudness = [[0, 0] for _ in range(self.max_polyphony)]
        self._deinited = False

    @property
    def pressed(self):
        return tuple(note for note in self._notes if not note._released)

    def _release(self):
        self._notes.clear()
        self.blocks.clear()
        self.waveform = None
        self.envelope = None

    def _start_note(self, note):
        envelope = note.envelope if note.envelope is not None else self.envelope
        if envelope is None:
            note._envelope_state = _audiodsp.EnvelopeState(
                self.sample_rate, False
            )
        else:
            note._envelope_state = _audiodsp.EnvelopeState(
                self.sample_rate, True, *envelope
            )
        note._envelope_seen = envelope
        note._released = False
        note._reset_filter()

    def _refresh_envelope(self, note):
        """Re-read ``note.envelope`` if it has been reassigned since the
        definition was built, keeping the running state (level, substep,
        phase) intact.

        The MicroPython and CircuitPython builds fetch a note's envelope on
        every render block, so assigning ``note.envelope`` takes effect at
        once - which is what makes one circuit shared by two voices with
        different decays behave correctly there. This target caches the
        definition in the state object at press time, so without this it
        would keep stepping the *previous* envelope and the two voices would
        swap decays. ``Envelope`` is an immutable namedtuple, so identity is
        a sound and cheap guard.
        """
        state = note._envelope_state
        if state is None:
            return
        envelope = note.envelope if note.envelope is not None else self.envelope
        if envelope is note._envelope_seen:
            return
        if envelope is None:
            state.set_definition(self.sample_rate, False)
        else:
            state.set_definition(self.sample_rate, True, *envelope)
        note._envelope_seen = envelope

    def _coerce(self, note):
        if not isinstance(note, int):
            return note
        # Integer notes retain their MIDI identity so the native-compatible
        # frequency table and the synthesizer's global waveform/envelope are
        # selected at render time.
        result = Note(midi_to_hz(note))
        result._midi_note = note
        return result

    def press(self, notes):
        # Mirrors CircuitPython's synthio_span_change_note exactly
        # (shared-module/synthio/__init__.c): a note already playing
        # re-enters ATTACK from its current level with its oscillator
        # phase intact (and its filter reset, per synthio_note_start); a
        # fresh press claims a free channel starting at phase zero; a
        # press with no free channel is REFUSED, never evicted. The old
        # code evicted the oldest note - and, on an at-cap re-press,
        # evicted a bystander and leaked a slot (issues #8/#9).
        if isinstance(notes, (int, Note)): notes = (notes,)
        for item in notes:
            note = self._coerce(item)
            if not isinstance(note, Note): raise TypeError("note must be int or Note")
            if note in self._notes:
                # A note still sounding swells back up from where it is,
                # keeping its oscillator phase. A note whose envelope has
                # already run down to 0 is finished - merely not collected
                # yet - and a press on it is a NEW hit, not a swell, so it
                # starts fresh with its phase reset. Making both targets
                # agree on this is what makes the *timing* of collection
                # unobservable: a silent note contributes nothing either
                # way, and the only thing that could ever depend on whether
                # its slot had been reclaimed was which of these two
                # branches a later press took.
                if (note._envelope_state is not None
                        and note._envelope_state.level == 0):
                    self._start_note(note)
                    note._accum = 0
                else:
                    note._released = False
                    if note._envelope_state is not None:
                        note._envelope_state.reattack()
                    note._reset_filter()
                continue
            if len(self._notes) >= self.max_polyphony:
                # No free channel. The oracle does NOT simply refuse here: at
                # this point find_channel_with_note (src/synthio/__init__.c:361)
                # is called with SYNTHIO_SILENCE, and it scans the channels
                # where the note is *not playing* - released ones, still
                # sounding out their tails - and takes the quietest. Only when
                # every channel is genuinely held does it return -1 and the
                # press is refused.
                #
                # This target used to hold a released note's slot for its
                # ENTIRE release tail, because _render below only drops a note
                # once its level reaches 0. Measured before this fix: with all
                # fourteen notes released, a fresh press was still refused, and
                # the slot came back 758 render blocks later. Driving rhodes
                # through the parity sequence, 117 of its 122 refusals happened
                # with NO key held at all - the engine was clogged by its own
                # decaying voices, not full. See docs/upstream-diff.md.
                victim = None
                quietest = None
                for other in self._notes:
                    if not other._released:
                        continue
                    state = other._envelope_state
                    level = state.level if state is not None else 0
                    if quietest is None or level < quietest:
                        victim, quietest = other, level
                if victim is None:
                    continue
                self._notes.remove(victim)
            self._start_note(note)
            note._accum = 0
            note._slot = self._free_slot()
            self._notes.append(note)

    def _free_slot(self):
        """The lowest channel index no sounding note holds.

        CircuitPython's `find_channel_with_note(SYNTHIO_SILENCE)` scans its
        channel array in order and takes the first free one, so admitting a
        note here has to pick the same index for the per-channel gate state to
        line up. A note that has left `_notes` has freed its channel, exactly
        as a channel set back to SYNTHIO_SILENCE has.
        """
        taken = {note._slot for note in self._notes}
        for index in range(self.max_polyphony):
            if index not in taken:
                return index
        # press() only reaches here having already evicted, so this cannot
        # happen; answer the last channel rather than raising out of a render.
        return self.max_polyphony - 1

    def release(self, notes):
        if isinstance(notes, (int, Note)): notes = (notes,)
        for item in notes:
            if isinstance(item, int):
                frequency = midi_to_hz(item)
                matches = [n for n in self._notes if abs(n.frequency - frequency) <= 1e-8]
            else:
                matches = [item] if item in self._notes else []
            for note in matches:
                note._released = True
                if note._envelope_state is not None:
                    note._envelope_state.release()

    def release_all(self): self.release(tuple(self._notes))
    def change(self, *, release=(), press=(), retrigger=()):
        self.release(release); self.press(press)
        for note in retrigger:
            if isinstance(note, Note): note._accum = 0
    release_then_press = change
    def release_all_then_press(self, notes): self.release_all(); self.press(notes)

    def note_info(self, note):
        note = self._coerce(note)
        if note not in self._notes or note._envelope_state is None:
            return EnvelopeState.RELEASE, 0.0
        return (EnvelopeState(note._envelope_state.state),
                note._envelope_state.level / 32767.0)

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        # CircuitPython preserves each oscillator accumulator here; reset only
        # invalidates the paired-channel buffer cache.

    def _render(self, sample_count, single_channel_output=False):
        _advance_blocks(self.sample_rate, 256)
        for block in self.blocks:
            _value(block)
        channels = 1 if single_channel_output else self.channel_count
        mixed = array("i", (0 for _ in range(sample_count * channels)))
        almost_one = 32767.0 / 32768.0

        def scaled(value, low=-almost_one, high=almost_one):
            value = min(high, max(low, _value(value))) * 32768.0
            return int(value + 0.5) if value >= 0 else int(value - 0.5)

        silenced = []
        for note in self._notes:
            slot = note._slot
            if note._envelope_state is None:
                self._start_note(note)
            # A voice at envelope level 0 is DONE: CircuitPython sets its
            # channel back to SYNTHIO_SILENCE and skips it here, before
            # rendering anything (shared-module/synthio/__init__.c, and
            # src/synthio/__init__.c does the same). Not a released-only check
            # -- any level of exactly 0 ends the voice.
            #
            # Rendering it anyway used to be harmless: pending loudness 0 times
            # any sample is 0, so it contributed silence. The zero-crossing
            # gate changed that. The gate holds the loudness the voice LAST
            # rendered at until the waveform crosses zero, so a note dropping
            # to level 0 kept sounding at its previous loudness for up to a
            # whole block -- audible material this target added and the native
            # builds did not. That was audiodsp#78: one block of
            # synthtools_acceptance's `bass`, the first after note_off.
            if note._envelope_state.level == 0:
                silenced.append(note)
                continue
            waveform = note.waveform if note.waveform is not None else self.waveform
            waveform = _DEFAULT_WAVEFORM if waveform is None else waveform
            length = len(waveform)
            start = max(0, min(length - 1, int(_value(note.waveform_loop_start))))
            end = max(start + 1, min(length, int(_value(note.waveform_loop_end))))

            if note._midi_note is not None and note.waveform is None:
                midi_note = note._midi_note
                octave = midi_note // 12
                dds_rate = (
                    self.sample_rate // 2
                    + (_MIDI_BASE_FREQUENCIES[midi_note % 12] * (end - start)
                       << (6 + octave))
                ) // self.sample_rate
            else:
                frequency_scaled = int(note.frequency * 65536.0 + 0.5)
                bend_scaled = scaled(note.bend, -12, 12)
                frequency_scaled = _audiodsp.pitch_bend(frequency_scaled, bend_scaled)
                dds_rate = (
                    self.sample_rate // 2 + frequency_scaled * (end - start)
                ) // self.sample_rate

            envelope_level = note._envelope_state.level
            if note._midi_note is not None:
                # Small-int MIDI notes in CircuitPython use the synthesizer's
                # envelope directly and do not pass through Note amplitude or
                # panning block inputs.
                loudness_left = loudness_right = envelope_level
            else:
                # panning > 0 attenuates the LEFT channel, matching
                # audiomixer.Mixer. CircuitPython 10.3.0 flipped this; before
                # it, synthio and Mixer panned in opposite directions.
                panning = scaled(note.panning)
                if panning >= 0:
                    left_pan, right_pan = 32767 - panning, 32768
                else:
                    left_pan, right_pan = 32768, 32767 + panning
                amplitude = scaled(note.amplitude)
                left_pan = (left_pan * amplitude) >> 15
                right_pan = (right_pan * amplitude) >> 15
                loudness_left = (envelope_level * left_pan) >> 15
                loudness_right = (envelope_level * right_pan) >> 15

            voice_data, note._accum = _audiodsp.oscillator_raw_i32(
                waveform, note._accum, dds_rate, start, end, sample_count,
            )

            # Ring modulation, mirroring the usermod's own stage: a second
            # oscillator multiplied into the voice, after the main
            # oscillator and before the filter. Needs both a non-zero
            # ring_frequency and a ring_waveform, exactly as the usermod
            # requires both ring_frequency_scaled and ring_waveform_buf.
            ring_waveform = note.ring_waveform
            if (note._midi_note is None and note.ring_frequency
                    and ring_waveform is not None):
                ring_length = len(ring_waveform)
                ring_start = max(0, min(ring_length - 1,
                                        int(_value(note.ring_waveform_loop_start))))
                ring_end = max(ring_start + 1,
                               min(ring_length,
                                   int(_value(note.ring_waveform_loop_end))))
                ring_scaled = int(note.ring_frequency * 65536.0 + 0.5)
                ring_bent = _audiodsp.pitch_bend(
                    ring_scaled, scaled(note.ring_bend, -12, 12))
                ring_dds_rate = (
                    self.sample_rate // 2 + ring_bent * (ring_end - ring_start)
                ) // self.sample_rate
                # Two guards, both from the usermod and both kept as written
                # there. The first bounds the rate against the RING table;
                # the second - easy to misread - bounds it against the MAIN
                # waveform's limit, and skips ringing entirely rather than
                # clamping.
                if ring_dds_rate > (ring_end << 16) // 2:
                    ring_dds_rate = 0
                if ring_dds_rate and ring_dds_rate <= (end << 16) // 2:
                    voice_data, note._ring_accum = _audiodsp.ring_multiply_i32(
                        voice_data, ring_waveform, note._ring_accum,
                        ring_dds_rate, ring_start, ring_end,
                    )

            if note.filter is not None:
                stages = (note.filter,) if isinstance(note.filter, Biquad) else tuple(note.filter)
                for stage, state in zip(stages, note._filter_states):
                    amplitude = 1.0 if stage.A is None else _value(stage.A)
                    voice_data = state.process_i32(
                        voice_data, stage.mode.value,
                        _value(stage.frequency), _value(stage.Q),
                        amplitude, self.sample_rate,
                    )
            # The loudness this note last actually rendered at. A change waits
            # for a zero crossing (CircuitPython 10.3.0); at the end of the
            # block the kernel forces active to pending, so what carries into
            # the next call is simply this block's pending pair.
            # A loudness change waits for a zero crossing. The kernel forces
            # active to pending at the end of the block, so what carries into
            # the next call is simply this block's pending pair.
            active = self._active_loudness[slot]
            contribution = _audiodsp.apply_loudness_i32(
                voice_data, loudness_left, loudness_right, channels,
                active[0], active[1],
            )
            active[0], active[1] = loudness_left, loudness_right
            voice = memoryview(contribution).cast("i")
            for index, value in enumerate(voice):
                mixed[index] += value

        # A silenced voice gets no envelope step, exactly as a channel set to
        # SYNTHIO_SILENCE is skipped by the advance loop upstream, and its
        # channel is free for the next press.
        for note in silenced:
            self._notes.remove(note)

        for note in tuple(self._notes):
            self._refresh_envelope(note)
            note._envelope_state.step(sample_count)

        output = _audiodsp.mixdown_i32(mixed, self.max_polyphony)
        # Two, as `synthio/__init__.h`'s `int16_t *buffers[2]` is.
        return self._publish(output, 2)

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        return GET_BUFFER_MORE_DATA, self._render(
            256, single_channel_output=single_channel_output)


class MidiTrack(Synthesizer):
    def __init__(self, buffer, tempo, *, sample_rate=11025, waveform=None, envelope=None):
        super().__init__(sample_rate=sample_rate, waveform=waveform, envelope=envelope)
        self.buffer = bytes(buffer)
        self._tempo = int(tempo)
        self._position = 0
        self._error_location = -1
        self._duration = 0
        self._start_parse()

    @property
    def error_location(self):
        return None if self._error_location < 0 else self._error_location

    @property
    def tempo(self):
        return self._tempo

    @tempo.setter
    def tempo(self, value):
        value = int(value)
        if value < 1:
            raise ValueError("tempo must be at least 1")
        self._duration = self._duration * self._tempo // value
        self._tempo = value

    def _record_error(self):
        self._error_location = self._position
        self._position = len(self.buffer)

    def _parse_note(self):
        if self._position + 1 >= len(self.buffer):
            self._record_error()
            return 0
        note = self.buffer[self._position]
        velocity = self.buffer[self._position + 1]
        self._position += 2
        if note > 127 or velocity > 127:
            self._record_error()
        return note

    def _decode_duration(self):
        delta = 0
        continued = True
        while continued and self._position < len(self.buffer):
            byte = self.buffer[self._position]
            self._position += 1
            delta = (delta << 7) | (byte & 0x7f)
            continued = bool(byte & 0x80)
        if continued:
            # The upstream decoder's do/while cursor reports one byte beyond
            # the truncated variable-length quantity.
            self._position += 1
            self._record_error()
        if self._tempo == 0:
            self._record_error()
            return 0
        return delta * self.sample_rate // self._tempo

    def _decode_until_pause(self):
        while self._position < len(self.buffer):
            kind = self.buffer[self._position] >> 4
            self._position += 1
            if kind == 8:
                self.release(self._parse_note())
            elif kind == 9:
                self.press(self._parse_note())
            elif kind in (10, 11, 14):
                self._parse_note()
            elif kind in (12, 13):
                if (self._position >= len(self.buffer) or
                        self.buffer[self._position] > 127):
                    self._record_error()
                else:
                    self._position += 1
            elif kind == 15:
                self._position = len(self.buffer)
            else:
                self._record_error()
            if self._position < len(self.buffer):
                self._duration = self._decode_duration()
            if self._duration:
                break

    def _start_parse(self):
        self._position = 0
        self._error_location = -1
        self._duration = self._decode_duration()
        if self._duration == 0 and self._position < len(self.buffer):
            self._decode_until_pause()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        super()._reset_buffer(single_channel_output, audio_channel)
        self._start_parse()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        duration = min(256, self._duration)
        self._duration -= duration
        output = self._render(
            duration, single_channel_output=single_channel_output)
        if self._duration == 0:
            if self._position == len(self.buffer):
                return 0, output
            self._decode_until_pause()
        return GET_BUFFER_MORE_DATA, output

    def _release(self):
        self.buffer = b""
        self._notes.clear()


def from_file(file, *, sample_rate=11025, waveform=None, envelope=None):
    stream = open(file, "rb") if isinstance(file, (str, bytes)) else file
    close = stream is not file
    try:
        header = stream.read(14)
        if len(header) != 14 or header[:12] != b"MThd\0\0\0\x06\0\0\0\x01": raise ValueError("invalid file")
        if header[12] & 0x80:
            division = -int.from_bytes(header[12:13], "big", signed=True) * header[13]
        else:
            division = int.from_bytes(header[12:14], "big") * 2
        chunk = stream.read(8)
        if chunk[:4] != b"MTrk": raise ValueError("invalid file")
        data = stream.read(int.from_bytes(chunk[4:], "big"))
        return MidiTrack(data, division, sample_rate=sample_rate, waveform=waveform, envelope=envelope)
    finally:
        if close: stream.close()


def midi_to_hz(note):
    note = float(note)
    # CircuitPython derives this from the first entry in its ninth-octave
    # MIDI table, rather than using 440 * 2**((note-69)/12).
    return 8372.0 * 2.0 ** (note / 12.0 - 10.0)


def voct_to_hz(ctrl):
    ctrl = float(ctrl)
    if not -11 <= ctrl <= 11: raise ValueError("ctrl must be between -11 and 11")
    return 8372.0 * 2.0 ** (ctrl - 7.0)


__all__ = ("Biquad", "FilterMode", "Math", "MathOperation", "MidiTrack", "Note", "EnvelopeState", "LFO", "Synthesizer", "from_file", "Envelope", "midi_to_hz", "voct_to_hz", "waveform_max_length", "lfo_tick")
