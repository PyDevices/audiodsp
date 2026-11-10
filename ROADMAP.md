# Roadmap

audiodsp is heading toward more of the audio graph on more boards, with the
audio pump holding up under live use such as a stompbox or a stage rig.

## Later

- An RP2 pump driver: the pump on the second core with an I2S (PIO) sink, so
  RP2 boards get the same off-thread audio as ESP32. See
  [docs/how-it-works/pump-ports.md](docs/how-it-works/pump-ports.md) for how a port plugs in.
- An LC3 codec module beside `audiomp3`, frame by frame: encode PCM to one
  frame, decode a frame (or a lost one) to PCM. Leaves room for LE Audio later.
- `audiopump.DriftResampler` converting down as well as up, with its filter's
  cutoff following the output rate, so a 48 kHz stream can play on a
  44.1 kHz output.

## Housekeeping

- `.pyi` stubs for the modules that lack one (`audiobiquad`, `audiodelays`,
  `audiodynamics`, `audioecho`, `audiofilters`, `audiofreeverb`, `audiograph`,
  `audioladder`, `audiomixer`, `audioshaper`, `audiospeed`, `audioverb`).
- The CircuitPython glue's folder takes a lasting name (`src/circuitpython_spike/`
  becomes `src/circuitpython/`).
- `audiocore`, `audiomp3`, `audiopump` and `synthio` report a `__revision__`
  like the other modules, so a board can say which source each was built from
  (today those four have none on MicroPython).

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/audiodsp/issues).
