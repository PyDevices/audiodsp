# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""The type stubs name what their modules really export.

Each native module has a stub, ``src/<module>/<module>.pyi``. An editor
completes from the stub and the interpreter runs the module, so a name in one
and not the other is either a completion that does not work or a feature that
does not complete. This test reads the names out of every stub and compares
them with ``dir()`` of the real module.

On MicroPython the comparison is exact. A module's public names, and the
public names of each of its classes, are the stub's, no more and no fewer.
Set ``MICROPYTHON`` to a unix build that has audiodsp built in; without it
the MicroPython half is skipped. A name that only some ports carry is listed in ``PORT_ONLY``.

On CPython the modules are Python wrappers around one extension and add
helpers of their own, and an attribute set in ``__init__`` is not in the
class, so the comparison is one way: everything the stub declares for a module
must exist, and so must every class and method it declares. Attributes and
properties are checked on MicroPython.

What counts as a name: anything that does not start with an underscore, and
``__enter__``, ``__exit__``, ``__version__`` and ``__revision__``. Type aliases
a stub defines for the checker start with an underscore for that reason.
"""

import ast
import importlib
import os
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# The modules that have a stub.
MODULES = (
    "audiocore", "audiofeatures", "audiomath", "audiometer", "audiomp3",
    "audiovorbis", "audiooscillator", "audiopump", "audioroute",
    "audiosampler", "audiomodal", "audioconvolve", "synthio",
)

KEPT_DUNDERS = ("__enter__", "__exit__")
MODULE_DUNDERS = ("__version__", "__revision__")

# Declared in a stub, absent from a MicroPython build that lacks the port
# feature. Each is a "Module.Class.name" and says why.
PORT_ONLY = {
    # Only where machine.USBDevice streams isochronous endpoints.
    "audiopump.Ring.from_usb": "MP_USBD_HAS_SINK",
}

# A class that is a tuple has the tuple's own count and index, and its fields
# are found by attribute, not in dir(). These are made with no arguments to
# look the fields up.
TUPLE_METHODS = ("count", "index")
TUPLE_FACTORIES = {"synthio.Envelope": "synthio.Envelope()"}


def public(name, dunders):
    return not name.startswith("_") or name in dunders


def read_stub(module):
    """The names a stub declares, as {"names": [...], "classes": {...}}."""
    path = SRC / module / (module + ".pyi")
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    names = set()
    classes = {}
    tuples = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names.add(node.name)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            names.add(node.target.id)
        elif isinstance(node, ast.ClassDef) and public(node.name, ()):
            names.add(node.name)
            members = set()
            for item in node.body:
                if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    members.add(item.name)
                elif isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
                    members.add(item.target.id)
            classes[node.name] = {m for m in members if public(m, KEPT_DUNDERS)}
            for base in node.bases:
                root = base.value if isinstance(base, ast.Subscript) else base
                if isinstance(root, ast.Name) and root.id == "tuple":
                    tuples.add(node.name)
    return {
        "names": {n for n in names if public(n, MODULE_DUNDERS)},
        "classes": classes,
        "methods": {
            node.name: {
                item.name for item in node.body
                if isinstance(item, ast.FunctionDef)
                and public(item.name, KEPT_DUNDERS)
                and not any(isinstance(d, ast.Name) and d.id == "property"
                            for d in item.decorator_list)
            }
            for node in tree.body
            if isinstance(node, ast.ClassDef) and public(node.name, ())
        },
        "tuples": tuples,
    }


# Run by MicroPython, which has no ast and no json worth trusting: it prints
# one repr per module that this file reads back with ast.literal_eval.
MICROPYTHON_PROBE = '''
import sys
KEPT = ("__enter__", "__exit__")
MODULE = ("__version__", "__revision__")
FACTORIES = %(factories)r
def public(n, keep):
    return not n.startswith("_") or n in keep
for name in sys.argv[1:]:
    mod = __import__(name)
    out = {"names": sorted(n for n in dir(mod) if public(n, MODULE)),
           "classes": {}, "fields": {}}
    for n in dir(mod):
        value = getattr(mod, n)
        if isinstance(value, type):
            out["classes"][n] = sorted(m for m in dir(value) if public(m, KEPT))
            factory = FACTORIES.get(name + "." + n)
            if factory:
                inst = eval(factory, {name: mod})
                out["fields"][n] = sorted(m for m in %(fields)r.get(name + "." + n, ())
                                          if hasattr(inst, m))
    print(repr(out))
'''


def run_micropython(binary, modules, stubs):
    fields = {
        module + "." + cls: sorted(stubs[module]["classes"][cls])
        for module in modules for cls in stubs[module]["tuples"]
    }
    probe = MICROPYTHON_PROBE % {"factories": TUPLE_FACTORIES, "fields": fields}
    proc = subprocess.run(
        [binary, "-c", probe] + list(modules), capture_output=True, text=True,
        timeout=120, check=False)
    if proc.returncode != 0:
        raise RuntimeError("MicroPython failed (%d):\n%s%s"
                           % (proc.returncode, proc.stdout, proc.stderr))
    found = {}
    lines = [line for line in proc.stdout.splitlines() if line.startswith("{")]
    for module, line in zip(modules, lines):
        found[module] = ast.literal_eval(line)
    return found


def compare_micropython(module, stub, real):
    """Problems for one module: empty when stub and module agree."""
    problems = []
    declared = set(stub["names"])
    present = set(real["names"])
    for name in sorted(declared - present):
        if module + "." + name not in PORT_ONLY:
            problems.append("%s: the stub declares %s and the module does not" % (module, name))
    for name in sorted(present - declared):
        problems.append("%s: the module has %s and the stub does not" % (module, name))
    for cls, want in sorted(stub["classes"].items()):
        have = set(real["classes"].get(cls, ()))
        if cls in stub["tuples"]:
            have = (have - set(TUPLE_METHODS)) | set(real["fields"].get(cls, ()))
        for name in sorted(want - have):
            if "%s.%s.%s" % (module, cls, name) not in PORT_ONLY:
                problems.append("%s.%s: the stub declares %s and the class does not" % (module, cls, name))
        for name in sorted(have - want):
            problems.append("%s.%s: the class has %s and the stub does not" % (module, cls, name))
    return problems


def compare_cpython(module, stub):
    problems = []
    mod = importlib.import_module(module)
    for name in sorted(stub["names"]):
        if not hasattr(mod, name):
            problems.append("%s: the stub declares %s and the module does not" % (module, name))
    for cls, methods in sorted(stub["methods"].items()):
        value = getattr(mod, cls, None)
        if not isinstance(value, type):
            continue
        for name in sorted(methods):
            if not hasattr(value, name) and "%s.%s.%s" % (module, cls, name) not in PORT_ONLY:
                problems.append("%s.%s: the stub declares method %s and the class does not" % (module, cls, name))
    return problems


STUBS = {module: read_stub(module) for module in MODULES}


class StubsMatchModules(unittest.TestCase):
    def test_every_module_has_a_stub_with_names(self):
        for module in MODULES:
            with self.subTest(module=module):
                self.assertTrue(STUBS[module]["names"], module)
                self.assertTrue(
                    (SRC / module / (module + ".pyi")).read_text(encoding="utf-8")
                    .startswith("# SPDX-FileCopyrightText"))

    @unittest.skipUnless(
        os.environ.get("MICROPYTHON") or shutil.which("micropython"),
        "set MICROPYTHON to a unix build with audiodsp built in")
    def test_micropython_dir_matches(self):
        binary = os.environ.get("MICROPYTHON") or shutil.which("micropython")
        found = run_micropython(binary, MODULES, STUBS)
        self.assertEqual(sorted(found), sorted(MODULES))
        problems = []
        for module in MODULES:
            problems += compare_micropython(module, STUBS[module], found[module])
        self.assertEqual(problems, [], "\n" + "\n".join(problems))

    def test_cpython_names_exist(self):
        try:
            importlib.import_module("_audiodsp")
        except ImportError:
            self.skipTest("the audiodsp extension is not built or installed")
        problems = []
        for module in MODULES:
            problems += compare_cpython(module, STUBS[module])
        self.assertEqual(problems, [], "\n" + "\n".join(problems))

    def test_the_comparison_catches_a_renamed_method(self):
        stub = read_stub("audiometer")
        stub["classes"]["Meter"].discard("levels")
        stub["classes"]["Meter"].add("level")
        real = {"names": sorted(stub["names"]), "classes": {
            "Meter": sorted((stub["classes"]["Meter"] - {"level"}) | {"levels"})},
            "fields": {}}
        problems = compare_micropython("audiometer", stub, real)
        self.assertEqual(len(problems), 2, problems)


if __name__ == "__main__":
    unittest.main()
