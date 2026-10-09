"""What CircuitPython builds of audiodsp may call from double-precision libm.

CircuitPython's RP2040 port links with the pico-sdk's ``--wrap`` for every
double-precision math function, but compiles only the SDK's ROM shims for
them: ``sqrt``, ``sin``, ``cos``, ``tan``, ``atan2``, ``exp``, ``log`` and
``sincos``. Every other wrapped function (``fmod``, ``floor``, ``pow``, ...)
has nothing to resolve to, so one call to it from a source CircuitPython
compiles and the firmware stops linking. audiodsp#210 was ``fmod`` in
``audiodsp_trig.c``.

The sources it checks are the ones ``circuitpython.mk`` builds: the bindings
for its modules and the ``src/shared/`` files it lists, with every header they
include from ``src/shared/``. Comments and string literals are stripped
first, so a comment may name a function without tripping it. The float
versions (``fmodf``, ``floorf``, ...) are all provided and are not checked.

A test built from a regex can quietly match nothing and pass for ever, so the
first test plants every forbidden call in a throwaway source and must catch
each one, beside a clean source that must pass.
"""

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
SRC = REPO / "src"

# pico-sdk's PICO_WRAP_DOUBLE_FLAGS, less the eight double_aeabi_rp2040.S and
# double_v1_rom_shim_rp2040.S define, which are the only double math sources
# CircuitPython's raspberrypi port compiles for the RP2040.
UNRESOLVED_ON_RP2040 = (
    "ldexp", "copysign", "trunc", "round", "floor", "ceil",
    "asin", "acos", "atan", "sinh", "cosh", "tanh",
    "asinh", "acosh", "atanh", "exp2", "log2", "exp10", "log10",
    "pow", "powint", "hypot", "cbrt", "fmod", "drem", "remainder",
    "remquo", "expm1", "log1p", "fma",
)

CALL_RE = re.compile(
    r"(?<![\w.>])(" + "|".join(UNRESOLVED_ON_RP2040) + r")\s*\(")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"(shared/[^"]+)"', re.MULTILINE)


def strip_comments_and_strings(text):
    """Blank comments and literals, keeping the line count."""
    def blank(match):
        return "\n" * match.group(0).count("\n")
    pattern = re.compile(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
        re.DOTALL)
    return pattern.sub(blank, text)


def circuitpython_sources(repo=REPO):
    """The .c files circuitpython.mk compiles, and the shared headers they reach."""
    mk = (repo / "circuitpython.mk").read_text()
    joined = mk.replace("\\\n", " ")
    modules = re.search(r"^AUDIODSP_CP_MODULES\s*:=\s*(.*)$", joined, re.M).group(1).split()
    shared = re.search(r"addprefix \$\(AUDIODSP_CP_SRC\)/shared/,(.*?)\)\s*$",
                       joined, re.M | re.S).group(1).split()
    spike = repo / "src" / "circuitpython_spike"
    files = []
    for module in modules:
        for kind in ("shared-bindings", "shared-module"):
            files += sorted((spike / kind / module).glob("*.c"))
    files += [repo / "src" / "shared" / name for name in shared]
    seen, queue = [], list(files)
    while queue:
        path = queue.pop(0)
        if path in seen or not path.exists():
            continue
        seen.append(path)
        for inc in INCLUDE_RE.findall(path.read_text()):
            queue.append(repo / "src" / inc)
    return seen, modules, shared


def forbidden_calls(paths):
    found = []
    for path in paths:
        text = strip_comments_and_strings(path.read_text())
        for lineno, line in enumerate(text.split("\n"), 1):
            for match in CALL_RE.finditer(line):
                found.append((path, lineno, match.group(1)))
    return found


class CheckerCanFail(unittest.TestCase):
    def test_every_planted_call_is_caught_and_a_clean_file_passes(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            planted = tmp / "planted.c"
            planted.write_text("".join(
                f"double f{i}(double x) {{ return {name}(x, x); }}\n"
                for i, name in enumerate(UNRESOLVED_ON_RP2040)))
            caught = {name for _, _, name in forbidden_calls([planted])}
            self.assertEqual(caught, set(UNRESOLVED_ON_RP2040))
            clean = tmp / "clean.c"
            clean.write_text(
                "// fmod(x, y) named in a comment\n"
                "/* floor(x) too */\n"
                'static const char *s = "pow(2, 3)";\n'
                "double g(double x) { return sqrt(x) + fmodf(x, 1) + my_floor(x)"
                " + s.round(x) + p->ceil(x); }\n")
            self.assertEqual(forbidden_calls([clean]), [])


class CircuitPythonSources(unittest.TestCase):
    def test_the_source_list_is_read(self):
        # An empty list would pass the next test without looking at anything.
        paths, modules, shared = circuitpython_sources()
        self.assertGreaterEqual(len(modules), 10)
        self.assertIn("audiodsp_trig.c", shared)
        self.assertIn(SRC / "shared" / "audiodsp_trig.c", paths)
        self.assertIn(SRC / "shared" / "audiodsp_trig.h", paths)
        for name in shared:
            self.assertTrue((SRC / "shared" / name).exists(), name)

    def test_no_double_libm_call_the_rp2040_build_cannot_resolve(self):
        paths, _, _ = circuitpython_sources()
        found = forbidden_calls(paths)
        self.assertEqual(
            [f"{p.relative_to(REPO)}:{n}: {name}()" for p, n, name in found], [],
            "CircuitPython's RP2040 build has no double-precision version of "
            "these, so the firmware would not link (audiodsp#210)")


if __name__ == "__main__":
    unittest.main()
