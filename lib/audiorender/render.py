# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""The block render loop: a composition in, a mixed master out.

Rendering happens a block at a time, and every block does the same three
things in the same order - move the transport to the head of the block,
deliver every event that falls inside it, then pull the block. Events are
delivered with the block's own start as their sample position, which is
what a plug-in host does; an instrument that resolves sample positions
finer than a block sees the same thing here as it does there.
"""

import time

import numpy as np

from .events import build_events
from .tempo import TempoMap
from .voices import Clock
from .wav import write_wav

#: Frames per block. The sidecar and every host this renders against use
#: 256, and block size is audible - it is the grid events land on.
BLOCK = 256


class Master:
    """A finished render: the audio, and the numbers a report is made of."""

    def __init__(self, composition, tempo, data, section_rms, bounds):
        self.composition = composition
        self.tempo = tempo
        self.data = data
        self.sample_rate = tempo.sample_rate
        #: {track name: [rms per section]}, before the master gain.
        self.section_rms = section_rms
        #: [(first sample, last sample)] per section.
        self.bounds = bounds

    @property
    def peak(self):
        return float(np.abs(self.data).max())


def render_track(track, composition, tempo, voice, total_frames,
                 patch=None, effects=(), clock=None, block=BLOCK):
    """One track's audio as float32 ``(total_frames, 2)`` in -1.0..1.0.

    `voice` is anything implementing the protocol in `.voices`, and
    `effects` are callables taking the rendered track as int16 bytes and
    returning something to pull the processed track from - a rack built
    from `audioeffects`, or a host's own effect runner.
    """
    events = build_events(track, composition, tempo, patch=patch, block=block)
    data = np.zeros((total_frames, 2), dtype=np.float32)
    cursor = 0
    next_event = 0
    while cursor < total_frames:
        frames = min(block, total_frames - cursor)
        if clock is not None:
            clock.move_to(cursor)
        while (next_event < len(events)
               and events[next_event][0] < cursor + frames):
            voice.deliver(events[next_event], cursor)
            next_event += 1
        raw = voice.pull_frames(frames)
        values = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        data[cursor:cursor + frames] = values.reshape(-1, 2) / 32768.0
        cursor += frames

    for effect in effects:
        pcm = (np.clip(data, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
        stage = effect(pcm)
        # Pulled in blocks like the track itself. Asking a stage for the
        # whole song in one call concatenates a growing bytes object and
        # turns a linear render into quadratic work on long tails.
        processed = np.zeros((total_frames, 2), dtype=np.float32)
        cursor = 0
        while cursor < total_frames:
            frames = min(block, total_frames - cursor)
            values = np.frombuffer(stage.pull_frames(frames), dtype=np.int16)
            processed[cursor:cursor + frames] = (
                values.astype(np.float32).reshape(-1, 2) / 32768.0)
            cursor += frames
        data = processed
    return data


def apply_mix(track, data, composition, tempo, block=BLOCK):
    """Fader moves and pan, in place; returns the same array.

    The fader is sampled per block rather than per sample, so a swell is a
    staircase of block-wide steps. That is what a host automation lane does
    too, and matching it matters more than being smooth.
    """
    total_frames = len(data)
    beats = np.array([tempo.beat_at_sample(sample)
                      for sample in range(0, total_frames, block)])
    gains = np.array([composition.track_gain(track, beat) for beat in beats],
                     dtype=np.float32)
    data *= np.repeat(gains, block)[:total_frames][:, None]
    pan = track["pan"]
    if pan:
        data[:, 0] *= min(1.0, 1.0 - pan)
        data[:, 1] *= min(1.0, 1.0 + pan)
    return data


def render(composition, voice_for, patch_for=None, effects_for=None,
           clock=None, sample_rate=None, block=BLOCK, out=None, stems=None):
    """Render every track of `composition` and mix them into a master.

    The three seams are callables, because how a caller turns a track into
    a sound is its own business:

    ``voice_for(track, clock)``
        the voice to render this track with (see `.voices`).
    ``patch_for(track)``
        ``{macro index: normalized value}`` for the macros the composition
        does not set - the instrument's own patch, not the middle of every
        range. Defaults to none, which resolves them all to 0.5.
    ``effects_for(track)``
        the track's effect stages, in order.

    `out` is where the per-track progress lines go; None keeps them quiet.
    """
    tempo = TempoMap.of(composition, sample_rate)
    rate = tempo.sample_rate
    if clock is None:
        clock = Clock(tempo)
    total_frames = int(composition.RENDER_SECONDS * rate)
    master = np.zeros((total_frames, 2), dtype=np.float32)
    bounds = [(int(composition.beats_to_seconds(first) * rate),
               int(composition.beats_to_seconds(last) * rate))
              for _name, first, last in composition.SECTIONS]

    if out is not None:
        out("%s: %.1f s song, %.1f s render, %d tracks"
            % (composition.TITLE, composition.SONG_SECONDS,
               composition.RENDER_SECONDS, len(composition.TRACKS)))

    section_rms = {}
    for track in composition.TRACKS:
        started = time.time()
        effects = tuple(effects_for(track)) if effects_for else ()
        data = render_track(
            track, composition, tempo, voice_for(track, clock), total_frames,
            patch=patch_for(track) if patch_for else None,
            effects=effects, clock=clock, block=block)
        raw_peak = float(np.abs(data).max())
        data = apply_mix(track, data, composition, tempo, block)
        section_rms[track["name"]] = [
            float(np.sqrt((data[first:last] ** 2).mean()))
            for first, last in bounds]
        master += data
        if out is not None:
            names = ", ".join(e["name"] for e in track.get("effects", ()))
            out("  %-14s raw_peak=%.3f mixed_peak=%.3f (%.1fs)%s"
                % (track["name"], raw_peak, float(np.abs(data).max()),
                   time.time() - started, " fx=" + names if names else ""))
        if stems is not None:
            write_wav(stems / (track["script"][:-3] + ".wav"), data, rate)

    master *= 10.0 ** (composition.MASTER_GAIN_DB / 20.0)
    return Master(composition, tempo, master, section_rms, bounds)
