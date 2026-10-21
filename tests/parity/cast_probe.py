"""audiomath.cast_into on every target: the same int16 from the same floats.

    cast_probe.py audiomath

The arithmetic is shared/audiodsp_cast.c on all three, so a line that differs
between two interpreters is a width or a compiler's choice, never intent. The
inputs sit where a cast goes wrong: exact halves either side of zero (round
half away, not to even and not toward zero), the floats just inside them, the
two rails and past them, infinities and NaN, at float32 and at float64.

On the native builds the probe also asks the heap. A host calls this every
block, and the reason it exists is that an array library's cast allocates a
temporary; a build where it allocates anything exits 1 here. CPython has no
heap counter to ask and skips that half.
"""

import sys
from array import array

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiomath"
math_module = __import__(MODULE)

INF = float("inf")
NAN = float("nan")

# Every input and scale is exact in float32, so a build whose Python float is
# float32 (CI's float cell) hands the C the same numbers CPython does, and no
# line depends on how an interpreter parses a decimal. Hence the powers of two.
ULP_HALF = 1 / 33554432  # 2**-25, one float32 step below 0.5
ULP_ONE = 1 / 16777216  # 2**-24, one step above 0.5 and below 1.0

HALVES = [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 32766.5, -32767.5, 0.0, -0.0]
NEAR = [0.5 - ULP_HALF, ULP_HALF - 0.5, 0.5 + ULP_ONE, -0.5 - ULP_ONE,
        1.5 - 2 * ULP_ONE, 2 * ULP_ONE - 1.5, 32767.25, 32767.75, -32768.25,
        -32768.75]
WIDE = [1.0, -1.0, 1.5, -1.5, 2.0, -2.0, 3.0, -3.0, INF, -INF, NAN,
        2.0 ** 100]


def ramp(count=257):
    """A ramp across +-2.2 in steps of 2**-17: past both rails at the vrack
    bridge's 16384, where the products land on every eighth (exact halves
    among them), and well past them at the default."""
    return [((i * 7919) % 576717 - 288358) / 131072 for i in range(count)]


def show(tag, typecode, values, scale=None):
    src = array(typecode, values)
    dst = array("h", bytes(2 * len(src)))
    if scale is None:
        math_module.cast_into(dst, src)
    else:
        math_module.cast_into(dst, src, scale)
    print("cast", tag, typecode, " ".join(str(v) for v in dst))


for typecode in ("f", "d"):
    show("halves", typecode, HALVES, 1.0)
    show("near", typecode, NEAR, 1.0)
    show("wide", typecode, WIDE)
    show("bridge", typecode, WIDE, 16384.0)
    show("ramp", typecode, ramp(), 16384.0)
    show("ramp-default", typecode, ramp())
    show("odd-scale", typecode, ramp(), 12345.75)

# Bytes of int16 samples are a destination too, and scale is a keyword.
raw = bytearray(8)
math_module.cast_into(raw, array("f", [0.25, -0.25, 1.0, -1.0]),
                      scale=4.0)
print("cast bytes", " ".join(str(b) for b in raw))

errors = []
for name, call in (
    ("short-dst", lambda: math_module.cast_into(array("h", [0, 0]),
                                                array("f", [0.0] * 3))),
    ("long-dst", lambda: math_module.cast_into(array("h", [0] * 4),
                                               array("f", [0.0] * 3))),
    ("int-src", lambda: math_module.cast_into(array("h", [0] * 3),
                                              array("h", [0] * 3))),
    ("float-dst", lambda: math_module.cast_into(array("f", [0.0] * 3),
                                                array("f", [0.0] * 3))),
):
    try:
        call()
        errors.append("%s:none" % name)
    except ValueError:
        errors.append("%s:ValueError" % name)
    except TypeError:
        errors.append("%s:TypeError" % name)
print("cast errors", " ".join(errors))

try:
    import gc
    mem_alloc = gc.mem_alloc
except (ImportError, AttributeError):
    mem_alloc = None

if mem_alloc is not None:
    src = array("f", ramp(256))
    wide = array("d", ramp(256))
    dst = array("h", bytes(512))
    cast = math_module.cast_into
    scale = 16384.0

    def blocks():
        for _ in range(100):
            cast(dst, src, scale)
            cast(dst, wide, scale)
            cast(dst, src)

    blocks()
    gc.collect()
    before = mem_alloc()
    blocks()
    grew = mem_alloc() - before
    if grew:
        print("cast_into allocated %d bytes over 300 calls" % grew)
        sys.exit(1)
