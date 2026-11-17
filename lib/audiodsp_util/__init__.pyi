# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""One number, the same number, on every target audiodsp runs on.

Round a setting to a single-precision float before handing it to a node, so a
board and a desktop derive the same value.
"""

__all__ = ("float32", "float32_bits")

def float32(value: float) -> float:
    """``value`` rounded to the nearest IEEE-754 single, as a float.

    The identity where a float is already a single, and a rounding where it is
    a double. A magnitude past single-precision range raises ``OverflowError``
    on CPython and gives an infinity on MicroPython and CircuitPython.
    """

def float32_bits(value: float) -> str:
    """``value`` as its IEEE-754 single bit pattern: eight lower-case hex
    characters, little-endian byte order, such as ``"f2c7bac0"``."""
