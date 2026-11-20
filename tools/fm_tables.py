# SPDX-FileCopyrightText: 2012 Google Inc.
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: Apache-2.0

"""Write src/shared/audiodsp_fm_tables.h, the lookup tables audiofm reads.

    python tools/fm_tables.py            # rewrite the header
    python tools/fm_tables.py --check    # fail if the header is stale

The sine and exp2 tables are built the way Music Synthesizer for Android
builds them at start-up (sin.cc and exp2.cc), in exact integer arithmetic
where it uses integers and in IEEE doubles where it uses doubles, so the
values match its runtime tables. Here they are written out once as `const`
arrays, so a board keeps them in flash instead of 16 KB of RAM. The fine
frequency table replaces a `log()` call per operator per note.
"""

import math
import pathlib
import sys

HEADER = pathlib.Path(__file__).resolve().parent.parent / "src" / "shared" / "audiodsp_fm_tables.h"

LG_N_SAMPLES = 10
N_SAMPLES = 1 << LG_N_SAMPLES


def sin_table():
    """Interleaved (delta, value) pairs, Q24, from an integer rotation."""
    dphase = 2 * math.pi / N_SAMPLES
    c = math.floor(math.cos(dphase) * (1 << 30) + 0.5)
    s = math.floor(math.sin(dphase) * (1 << 30) + 0.5)
    r = 1 << 29
    u = 1 << 30
    v = 0
    table = [0] * (N_SAMPLES * 2)
    for i in range(N_SAMPLES // 2):
        table[(i << 1) + 1] = (v + 32) >> 6
        table[((i + N_SAMPLES // 2) << 1) + 1] = -((v + 32) >> 6)
        t = (u * s + v * c + r) >> 30
        u = (u * c - v * s + r) >> 30
        v = t
    for i in range(N_SAMPLES - 1):
        table[i << 1] = table[(i << 1) + 3] - table[(i << 1) + 1]
    table[(N_SAMPLES << 1) - 2] = -table[(N_SAMPLES << 1) - 1]
    return table


def exp2_table():
    """Interleaved (delta, value) pairs of 2**x over one octave, Q30."""
    inc = math.exp2(1.0 / N_SAMPLES)
    y = float(1 << 30)
    table = [0] * (N_SAMPLES * 2)
    for i in range(N_SAMPLES):
        table[(i << 1) + 1] = math.floor(y + 0.5)
        y *= inc
    for i in range(N_SAMPLES - 1):
        table[i << 1] = table[(i << 1) + 3] - table[(i << 1) + 1]
    table[(N_SAMPLES << 1) - 2] = (1 << 31) - table[(N_SAMPLES << 1) - 1]
    return table


def fine_table():
    """Fine frequency 0..99 as a Q24 log2 offset: log2(1 + fine / 100)."""
    return [0] + [math.floor(24204406.323123 * math.log(1 + 0.01 * fine) + 0.5)
                  for fine in range(1, 100)]


def emit(name, values, per_line=6):
    lines = ["static const int32_t %s[%d] = {" % (name, len(values))]
    for start in range(0, len(values), per_line):
        chunk = values[start:start + per_line]
        lines.append("    " + ", ".join("%d" % value for value in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def render():
    for value in sin_table() + exp2_table() + fine_table():
        assert -(1 << 31) <= value < (1 << 31), value
    parts = [
        "// Lookup tables for audiodsp_fm.c, written by tools/fm_tables.py.",
        "// Do not edit by hand: change the script and run it again.",
        "//",
        "// Copyright 2012 Google Inc.",
        "// Copyright 2026 Brad Barnett",
        "//",
        "// Licensed under the Apache License, Version 2.0 (the \"License\");",
        "// you may not use this file except in compliance with the License.",
        "// You may obtain a copy of the License at",
        "//",
        "//      http://www.apache.org/licenses/LICENSE-2.0",
        "//",
        "// Unless required by applicable law or agreed to in writing, software",
        "// distributed under the License is distributed on an \"AS IS\" BASIS,",
        "// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.",
        "// See the License for the specific language governing permissions and",
        "// limitations under the License.",
        "//",
        "// SPDX-License-Identifier: Apache-2.0",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#define AUDIODSP_FM_TABLE_LG_N %d" % LG_N_SAMPLES,
        "#define AUDIODSP_FM_TABLE_N %d" % N_SAMPLES,
        "",
        "// One sine cycle, %d steps, as (delta, value) pairs in Q24." % N_SAMPLES,
        emit("audiodsp_fm_sin_table", sin_table()),
        "",
        "// One octave of 2**x, %d steps, as (delta, value) pairs in Q30." % N_SAMPLES,
        emit("audiodsp_fm_exp2_table", exp2_table()),
        "",
        "// Fine frequency 0..99 as log2(1 + fine / 100) in Q24.",
        emit("audiodsp_fm_fine_table", fine_table()),
        "",
    ]
    return "\n".join(parts)


def main():
    text = render()
    if "--check" in sys.argv[1:]:
        if HEADER.read_text() != text:
            print("%s is stale: run tools/fm_tables.py" % HEADER)
            return 1
        print("%s is current" % HEADER.name)
        return 0
    HEADER.write_text(text)
    print("wrote %s" % HEADER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
