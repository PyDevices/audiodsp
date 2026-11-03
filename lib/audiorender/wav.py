# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Writing a render out as a 16-bit stereo WAV."""

import wave

import numpy as np


def write_wav(path, data, sample_rate):
    """Write float `data` shaped ``(frames, 2)`` as 16-bit stereo PCM.

    Clipped rather than normalized: a render that came out too hot is a
    thing to fix in the mix, and quietly turning it down would hide it.
    """
    pcm = (np.clip(data, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(int(sample_rate))
        handle.writeframes(pcm.tobytes())
