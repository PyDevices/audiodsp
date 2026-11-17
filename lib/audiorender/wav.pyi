# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Write a render to a WAV file."""

from os import PathLike
from typing import Any

def write_wav(path: str | PathLike[str], data: Any, sample_rate: int) -> None:
    """Write float `data` shaped ``(frames, 2)`` as 16-bit stereo PCM.

    Samples are clipped to -1.0..1.0, not normalized.
    """
