# Where audiodsp differs from CircuitPython

audiodsp's `audiocore`, `synthio`, `audiomixer`, `audiospeed`, `audiodelays`,
`audiofilters`, `audiofreeverb` and `audiomp3` render the same bytes as
CircuitPython's, at the release named on the `circuitpython` line of
`DEPENDENCIES.lock` (11.0.0-alpha.2). A program written for CircuitPython runs
here unchanged. This page lists each place where it doesn't behave the same,
what you'll notice, and what to do about it.

Most of the differences are fixes: a pull that hangs on CircuitPython returns
here, and a click or a lost note there plays cleanly here. If your program also
has to run on a stock CircuitPython board, the **On CircuitPython** line under
each entry says what to avoid.

The modules CircuitPython doesn't have at all, such as `audiobiquad` and
`audiopump`, are listed in [The modules](../reference/modules.md). The
measurements behind every entry here, and how the port is kept in step with
upstream, are contributor material:
[the upstream record](https://github.com/PyDevices/audiodsp/blob/main/docs/dev/upstream-record.md)
and [Keeping in step with CircuitPython](https://github.com/PyDevices/audiodsp/blob/main/docs/dev/upstream-and-parity.md).

## Things you can do here that CircuitPython can't

### Ramp a Mixer voice's level

`audiomixer.Mixer.level_ramp` sets how many frames a level or pan change takes.
The voice then walks to the new value in a straight line instead of stepping,
so a crossfade moved in one call doesn't click.

```python
mixer.level_ramp = 960          # 20 ms at 48 kHz
mixer.voice[0].level = 0.0      # walks down over the next 960 frames
mixer.voice[1].level = 1.0      # walks up over the same frames
```

CircuitPython holds a level change until the voice's next zero crossing. That
keeps a single waveform clean, but it doesn't help a crossfade: the dry and wet
voices cross zero at different moments, and a voice with a DC offset may not
cross at all. Left at 0, the default, the Mixer is CircuitPython's.

**On CircuitPython:** `level_ramp` doesn't exist. Fade with many small level
moves instead.

### Ramp a Flanger's mix

`audiodelays.Flanger.mix_ramp` does the same for `mix`. Without it, taking the
mix from 0 to 1 in one call steps the output by the whole gap between the dry
and wet signals.

```python
flanger.mix_ramp = 960
flanger.mix = 1.0               # walks up over the next 960 frames
```

The default is 0, which behaves as CircuitPython does.

**On CircuitPython:** `mix_ramp` doesn't exist.

### Give a note a steeper filter

`synthio.Note.filter` also takes a tuple or list of up to four `Biquad`s,
applied one after another. Each stage adds 12 dB per octave, which a noise
voice often needs. One `Biquad` behaves exactly as it does on CircuitPython.

**On CircuitPython:** pass a single `Biquad`.

### Open a file by name

`audiocore.WaveFile` and `synthio.from_file` take a path as well as an open
file.

**On CircuitPython:** open the file yourself and pass the file object.

### Play more notes at once

A `synthio.Synthesizer` here has 64 voices. CircuitPython's own unix build has
14, and its boards vary. Instruments that press several notes per key (an
organ that stacks five partials, say) lose chord tones at 14: the fourth key of
a four-key chord simply doesn't sound. At 64 they're all there.

The mix-down limiter is set for the voice count, so very loud material that
goes over its knee (about ±28000) is squashed a little harder here, by about a
third of a decibel at ten full-scale voices. Below the knee, and with fewer
notes than CircuitPython's limit, the output is the same to the byte.

**On CircuitPython:** a press with no free voice is refused. Keep chords within
the board's voice count.

### Count bytes in `get_buffer`

`audiocore.get_buffer()` returns a view of bytes, so `len()` of what it hands
back is a length in bytes, the same unit the audio protocol counts in.
CircuitPython returns a view typed by sample width, where `len()` counts
samples: half the byte count for 16-bit audio.

**On CircuitPython:** multiply by the sample width yourself.

## Pulls that hang on CircuitPython return here

On CircuitPython each of these pulls goes round its loop forever. On a board,
that stops the audio and everything waiting on it. Here each one returns.

- **An effect whose source has nothing to give.** `MultiTapDelay`,
  `PitchShift`, `GranularPitchShift` and `Flanger` fill the rest of the block
  with silence when their source has nothing this time, and ask it again on the
  next pull. A looping source that comes back empty straight after a reset has
  nothing at all, so they let it go (`playing` goes false).
- **A looped `Echo` on a source that has ended.** For example, an
  `audioroute.Port` pointed at a finished sound. `Echo` lets the source go and
  rings out what its delay line holds.
- **A looping sample shorter than one 32-bit word in a `Mixer`.** A one-frame
  mono `RawSample` played with `loop=True` measures zero words, so
  CircuitPython never finishes the block. Here `play(loop=True)` raises
  `ValueError("A looped sample must fill at least one 32-bit word")`. Make the
  loop at least two frames long. audiodsp doesn't pad it for you, because
  padding a one-frame loop to two halves its pitch.

**On CircuitPython:** don't loop a sample under one 32-bit word, and don't feed
these effects a source that can come back empty.

## Resets

`audiocore.reset_buffer()` on a node clears what the node holds of its own (its
history, its tail, its filter memory) and keeps the source frames it has
already taken. CircuitPython's own effects do the same. Three nodes differ.

- **`Mixer`.** On CircuitPython a reset stops every voice for good, and every
  effect's `play()` resets what it's given. So an effect chained straight after
  a Mixer is silent there. Here a reset leaves every voice playing where it
  was.

  **On CircuitPython:** keep a Mixer as the last node before the output, not
  in the middle of a chain.
- **`PitchShift` and `GranularPitchShift`.** A reset here leaves the node
  exactly as a new one starts. On CircuitPython, `PitchShift` keeps its read
  and write positions, so the splice after a reset falls where the old node had
  drifted to, and `GranularPitchShift` carries on its random grain offsets
  instead of starting them again. A change of `semitones` isn't a reset and
  still keeps the splice where it is, because moving it would click.
- **`MultiTapDelay`.** A reset puts both write heads back at the top. On
  CircuitPython, a later shortening of the delay time can then play an echo a
  fresh node doesn't.

## Sounds that come out differently

Each of these is a CircuitPython bug that audiodsp fixes. A program sounds the
same on both until it reaches the case described.

- **`PitchShift` keeps the shifted note.** Upstream crossfades to audio a whole
  window away wherever it lands. When that lands out of phase, the crossfade
  cancels the note, and about half the notes from C2 to C6 lose their shifted
  pitch by 3 dB or more, some almost entirely. Here the incoming side is moved,
  by up to a 50th of a second, to where it matches the outgoing side. Every held
  note from C2 to C6 keeps its shifted pitch within 1.5 dB. A chord can still
  lose 4 to 5 dB, because a splice can only match one note's phase. Silence and
  a single click splice where upstream's do. This applies to 16-bit signed
  audio, which is what every effect class uses.
- **A frozen `PitchShift` loop doesn't click on each lap.** Where the read meets
  the write, upstream plays the oldest frame in the window; here it plays the
  frame that follows the window's newest.
- **`MultiTapDelay` keeps its stereo channels apart.** On CircuitPython, a
  stereo source that hands over an odd number of samples swaps left and right
  for the rest of the stream.
- **`Echo` and `MultiTapDelay` don't click when the delay grows back.** Shorten
  the delay while the write head is past the new end, and upstream writes one
  stray sample into the unused part of the line. When the delay grows again it
  plays as a single-sample click.
- **`Flanger` doesn't overflow on loud material.** Upstream's interpolation
  overflows a 32-bit integer when adjacent samples sit at opposite rails, above
  about ±26000 alternating.
- **`audiospeed` rounds its rate to the nearest step.** CircuitPython truncates
  a rate to 16.16 fixed point, so a rate a hair below a step loses a whole
  step. At 44.1 kHz, a `SpeedChanger` pair meant to cancel out (a rate and its
  inverse) can then move a full-scale tone by most of its range. Here it
  cancels.
- **`audiospeed` carries its position across source buffers.** CircuitPython
  restarts its fractional position at every new buffer from the source, so a
  `SpeedChanger` used as a sample-and-hold renders differently depending on the
  source's buffer size, and puts images where a sample-and-hold shouldn't. Here
  the output is the same for any buffer size.

**On CircuitPython:** these play as described above. Use whole-step rates in
`audiospeed`, and keep loud alternating material out of `Flanger`.

### Low-frequency filters settle at zero

`synthio.Biquad`, and so `audiofilters.Filter` and a note's filter, run the
arithmetic CircuitPython has on its `main` branch, which no release carries
yet. A low-pass below about 100 Hz on CircuitPython 11.0.0-alpha.2 settles on a
constant after the sound stops, and a 200 Hz low shelf holds a small DC offset.
Here a low-pass settles at exact zero at 400 Hz and above, and within a few
counts of it at 100 Hz and 40 Hz, which you can't hear.

For a filter whose tail must reach exact zero at any frequency, use
`audiobiquad`, which keeps its state in floating point. Above a few hundred
hertz the two differ by a few LSB, so a sketch ported from CircuitPython can
keep its `audiofilters.Filter`.

**On CircuitPython:** use `Filter` above about 200 Hz, or build the low part
another way.

## Kept as CircuitPython has them, though they look wrong

These match CircuitPython on purpose, so code ported either way sounds the
same.

- **`audiofilters.Phaser`'s tail settles on a constant**, not zero: −8 at
  48 kHz, −9 at 44.1 kHz stereo, −5 at 22.05 kHz. The value depends on the
  rate and channel count. If you need a tail that reaches zero, build the
  phaser from `audiobiquad.AllPass`.
- **`audiofilters.Distortion` ignores `drive` in `OVERDRIVE` mode.** The curve
  is fixed; `CLIP` and `WAVESHAPE` use `drive`. To drive the overdrive harder,
  raise `pre_gain`.

## When the audio pump plays the graph

`audiopump` pulls the graph on a thread with no interpreter, which CircuitPython
doesn't have, so none of these can happen there.

- **A file-reading source can't be pumped.** `WaveFile` and `MP3Decoder` read
  their file while they play, and the pump refuses them with a fault code
  wherever they sit in the graph. Decode into an `audiopump.Ring` from the
  interpreter instead ([Play computed audio](../guides/play-computed-audio.md)).
- **A pull never raises.** Inside the graph, a released or wrong source makes
  a silent block and a fault code. `audiocore.get_buffer()` and
  `reset_buffer()`, called from your script, still raise as they do on
  CircuitPython.
- **No finalisers.** CircuitPython 11 calls `deinit()` on an audio object when
  it's garbage collected. Here only `MP3Decoder` does, because a finaliser
  could release a node while the pump is still pulling it. Call `deinit()`, or
  use `with`, when you're done with a node.
- **A released effect lets go of its source.** `deinit()` on the ported effects
  also drops the source, so the chain behind it can be collected.
- **Changing a node while it plays is safe.** A `press`, a new filter, a tap
  change or a `deinit()` takes effect between two blocks, never in the middle
  of one.

## The same bytes on every target

audiodsp's CircuitPython-compatible modules render the same bytes on
MicroPython, CPython and CircuitPython. A setting computed in Python can still
differ, because a Python float is a double on a desktop and a single on a
board. Put a derived setting through `audiodsp_util.float32()` and every
target holds the number a board would. [The correctness
standard](correctness-standard.md) says how each module is held to this.
