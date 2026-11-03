# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

import synthio, audiocore
try:
    from ulab import numpy as np
except ImportError:
    import numpy as np

RATE = 48000
# A drum-shaped envelope: decays to silence on its own and is never released.
env = synthio.Envelope(attack_time=0.001, decay_time=0.02,
                       release_time=0.05, attack_level=1.0, sustain_level=0.0)
synth = synthio.Synthesizer(sample_rate=RATE, envelope=env)
buf = np.zeros(256, dtype=np.int16)

n = 0
notes = []
while True:                      # fill every channel
    note = synthio.Note(frequency=220 + 10 * n, envelope=env)
    synth.press(note)
    if len(synth.pressed) <= n:  # refused: we are at the ceiling
        break
    notes.append(note); n += 1
    if n > 128:
        break
print("channels filled:", n)

for _ in range(200):             # let every envelope decay to zero
    audiocore.get_buffer(synth)
print("still pressed after decay:", len(synth.pressed))

extra = synthio.Note(frequency=1000, envelope=env)
synth.press(extra)
accepted = extra in synth.pressed
print("a further press after decay is accepted:", accepted)
