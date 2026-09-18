"""One number, the same number, on every target audioif runs on.

``import audioif_util`` -- a pure-Python tier with no dependencies beyond
``struct``, so it imports on CPython, MicroPython and CircuitPython alike.
Nothing here touches the DSP; it exists so the numbers *handed* to the DSP
stop depending on which interpreter derived them.

## Why this is needed at all

Python's float is the interpreter's ``mp_float_t``. On CPython and on a
desktop MicroPython that is a **double**; on an ESP32-P4, an ESP32-S3, an
RP2040 and any MicroPython built with ``MICROPY_FLOAT_IMPL_FLOAT`` it is a
**single**. So::

    node.mix = 0.35

is not one setting. It is ``0.34999999403953552`` on a board and
``0.34999999999999998`` on a desktop, and a node that runs its dry/wet blend
from it renders different bytes on the two. That is not a bug in the node,
and no amount of care inside the kernel can fix it: the number was already
two different numbers before it arrived.

`docs/correctness-standard.md` holds our nodes to every target rendering
them identically, so a setting derived in Python arithmetic passes through
:func:`float32` before it reaches a node. On a single-precision target that
call is the identity. On a double one it rounds to the value the board would
have held. Both then agree, which is the whole point.

The same rule is what audiocomponents#75 needs on the class side: a class
computing ``360 * 4 ** (macro / 127)`` for a filter frequency is deriving a
setting in Python, and its board and its desktop land a ULP apart until that
derivation ends in :func:`float32`.

## And why printing needs its own function

:func:`float32_bits` is for output rather than settings. ``"%.6f" % value``
is not one string across interpreters even when ``value`` is bit-for-bit the
same float32: six decimals of a single-precision number is seven significant
digits, and MicroPython's single-precision formatter is not correctly
rounded that far -- it prints ``-5.836908`` where the value is
``-5.836907386779785``. A probe that prints a float at that width is
comparing formatters, not DSP. The bit pattern is exact everywhere and
strictly more sensitive than six decimals, so it is what the parity probes
print.
"""

import struct

__all__ = ("float32", "float32_bits")


def float32(value):
    """``value`` rounded to the nearest IEEE-754 single, as a float.

    The identity on any interpreter whose float already *is* a single, and a
    rounding on one whose float is a double -- which is exactly what makes
    the result the same number on both.

    Idempotent: ``float32(float32(x)) == float32(x)`` everywhere.

    **One stated limit, measured rather than assumed.** A magnitude past
    single-precision range is the one input on which the three runtimes do
    not agree: CPython's ``struct`` raises ``OverflowError`` and
    MicroPython's and CircuitPython's return an infinity. Keep settings
    inside the range a board can hold -- every audio setting is -- and this
    never arises; a value out there is a defect on the board too, and one
    that would render as silence rather than as an error.
    """
    return struct.unpack("<f", struct.pack("<f", value))[0]


def float32_bits(value):
    """``value`` as its IEEE-754 single bit pattern, lower-case hex.

    Eight characters, little-endian byte order, e.g. ``"f2c7bac0"`` for
    -5.8369074. Exact on every interpreter, unlike a decimal conversion of
    the same number, so it is what a cross-target probe prints for a float
    it has to compare.
    """
    return struct.pack("<f", value).hex()
