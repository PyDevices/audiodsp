#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Check each module's type stub against the C bindings MicroPython builds.

The stubs in ``src/<module>/<module>.pyi`` describe the modules built into
MicroPython, and ``tools/check_stubtest.py`` holds the CPython twins to the
same stubs. Between the two checks, a call that works on one interpreter and
raises on the other fails one of them.

This one reads the C sources, not a build. For every function and method a
module's globals table or a class's locals table names, it works out the
arguments from the ``MP_DEFINE_CONST_FUN_OBJ_*`` macro and, for a function
that takes keywords, from its ``mp_arg_t`` table. Then it compares them with
the stub and reports:

- a name the C table has and the stub lacks, or the other way round;
- an argument one side takes by name and the other only by position;
- an argument one side makes keyword-only and the other doesn't;
- an argument that is required on one side and optional on the other;
- a default that is ``None`` on one side and something else on the other.

A function whose arguments the sources don't show in a form this reads (one
that walks its arguments by hand, say) is counted as not checked; ``-v`` names
them. ``tools/stubs/bindings_allowlist.txt`` lists what this finds and doesn't
fix, each with its reason, and an entry that matches nothing fails the run.

Run it from a checkout. It needs no build::

    python3 tools/check_bindings.py              # every module
    python3 tools/check_bindings.py audiopump    # one module
    python3 tools/check_bindings.py -v           # also name what wasn't checked
"""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
ALLOWLIST = ROOT / "tools" / "stubs" / "bindings_allowlist.txt"

#: Folders under src/ that hold no MicroPython module bindings.
NOT_BINDINGS = {"circuitpython", "cpython", "shared", "cp_compat"}

#: Dunder names a stub may give a class that its locals table never holds.
IMPLICIT = {"__new__", "__repr__", "__str__", "__hash__", "__eq__", "__iter__",
            "__next__", "__len__", "__getitem__", "__setitem__", "__call__",
            "__bool__", "__class_getitem__", "__init_subclass__"}

ARG_ENTRY = re.compile(
    r"\{\s*MP_QSTR_(\w+)\s*,\s*([A-Z_ |]+?)\s*(?:,\s*\{([^{}]*)\})?\s*\}")


class Param:
    """One argument: its name, how it may be passed, and its default."""

    __slots__ = ("name", "kind", "required", "default_none")

    def __init__(self, name, kind, required, default_none=None):
        self.name = name  # None where C takes it by position only
        self.kind = kind  # "posonly", "pos" or "kwonly"
        self.required = required
        self.default_none = default_none  # True, False, or None for unknown


class Sig:
    """What a callable accepts."""

    def __init__(self, params, varargs=False, open_kw=False):
        self.params = params
        self.varargs = varargs
        # True when C walks its keywords by hand: which names it reads is
        # spread over helpers, so only the positional part is compared.
        self.open_kw = open_kw


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def parse_arg_table(body: str) -> list[Param]:
    params = []
    for name, flags, default in ARG_ENTRY.findall(body):
        flags = {f.strip() for f in flags.split("|")}
        required = "MP_ARG_REQUIRED" in flags
        kind = "kwonly" if "MP_ARG_KW_ONLY" in flags else "pos"
        none = None
        if not required:
            d = default.replace(" ", "")
            if "u_obj" in d:
                none = "MP_ROM_NONE" in d or "mp_const_none" in d
            elif "u_int" in d or "u_bool" in d or "u_rom_obj" in d:
                none = "MP_ROM_NONE" in d
            elif not d:
                # An omitted default is a NULL object, which the functions
                # here treat as "not given": a stub writes that as None.
                none = True
        params.append(Param(name, kind, required, none))
    return params


class CSources:
    """The binding tables of a set of C files, read with regular expressions."""

    def __init__(self, files):
        text = "\n".join(strip_comments(f.read_text(errors="replace")) for f in files)
        self.text = text
        self.funobj = {}
        for kind, args in re.findall(
                r"MP_DEFINE_CONST_FUN_OBJ_(\d|VAR_BETWEEN|VAR|KW)\s*\(([^;]*?)\)\s*;", text):
            parts = [a.strip() for a in args.split(",")]
            self.funobj[parts[0]] = (kind, parts[1:])
        self.static = dict(re.findall(
            r"MP_DEFINE_CONST_(?:STATIC|CLASS)METHOD_OBJ\s*\(\s*(\w+)\s*,\s*[^&]*&(\w+)", text))
        self.props = set(re.findall(r"MP_PROPERTY_(?:GETTER|GETSET)\s*\(\s*(\w+)", text))
        self.props |= set(re.findall(r"mp_obj_property_t\s+(\w+)", text))
        self.tables = {}
        for name, body in re.findall(
                r"mp_rom_map_elem_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\}\s*;", text, flags=re.S):
            self.tables[name] = re.findall(
                r"MP_ROM_QSTR\s*\(\s*MP_QSTR_(\w+)\s*\)\s*,\s*([^}]*)\}", body)
        self.dicts = dict(re.findall(
            r"MP_DEFINE_CONST_DICT\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)", text))
        self.types = {}
        for args in re.findall(r"MP_DEFINE_CONST_OBJ_TYPE\s*\((.*?)\)\s*;", text, flags=re.S):
            parts = [a.strip() for a in args.split(",")]
            info = {"name": parts[1].replace("MP_QSTR_", "")}
            slots = parts[3:]
            for key, value in zip(slots[0::2], slots[1::2]):
                info[key] = value.strip("&() ")
            self.types[parts[0]] = info
        self.arg_tables = {name: parse_arg_table(body) for name, body in re.findall(
            r"mp_arg_t\s+(\w+)\s*\[\s*\w*\s*\]\s*=\s*\{(.*?)\}\s*;\s*\n", text, flags=re.S)}
        self.modules = {}
        for mod, obj in re.findall(r"MP_REGISTER_MODULE\s*\(\s*MP_QSTR_(\w+)\s*,\s*(\w+)", text):
            m = re.search(r"mp_obj_module_t\s+%s\s*=\s*\{.*?\.globals\s*=\s*\(mp_obj_dict_t\s*\*\)\s*&(\w+)"
                          % obj, text, flags=re.S)
            if m:
                self.modules[mod] = m.group(1)

    def body(self, fn: str):
        m = re.search(r"\b%s\s*\([^;{]*\)\s*\{" % re.escape(fn), self.text)
        if not m:
            return None
        depth, i = 1, m.end()
        while depth and i < len(self.text):
            depth += {"{": 1, "}": -1}.get(self.text[i], 0)
            i += 1
        return self.text[m.end():i]

    def kw_table(self, fn: str):
        body = self.body(fn)
        if body is None:
            return None
        local = re.search(r"mp_arg_t\s+\w+\s*\[\s*\w*\s*\]\s*=\s*\{(.*?)\}\s*;", body, flags=re.S)
        if local:
            return parse_arg_table(local.group(1))
        used = re.search(r"mp_arg_parse_all\w*\s*\([^;]*?MP_ARRAY_SIZE\s*\(\s*(\w+)\s*\)", body, flags=re.S)
        if used and used.group(1) in self.arg_tables:
            return self.arg_tables[used.group(1)]
        return None

    def signature(self, ptr: str, method: bool):
        """The `Sig` of the function object ``ptr``, or None when unreadable."""
        if ptr in self.static:
            ptr, method = self.static[ptr], False
        if ptr not in self.funobj:
            return None
        kind, args = self.funobj[ptr]
        skip = 1 if method else 0
        if kind.isdigit():
            return Sig([Param(None, "posonly", True) for _ in range(int(kind) - skip)])
        if kind in ("VAR", "VAR_BETWEEN"):
            lo = int(args[0]) - skip
            if kind == "VAR":
                return Sig([Param(None, "posonly", True) for _ in range(lo)], varargs=True)
            hi = int(args[1]) - skip
            return Sig([Param(None, "posonly", i < lo) for i in range(hi)])
        table = self.kw_table(args[1])
        if table is None:
            return None
        if method and table and table[0].name == "self":
            table = table[1:]  # a table that names self, parsed from n_args
        return Sig(table)

    def make_new(self, fn: str):
        table = self.kw_table(fn)
        if table is not None:
            return Sig(table)
        body = self.body(fn)
        if body is None:
            return None
        m = re.search(r"mp_arg_check_num\w*\s*\(\s*n_args\s*,\s*n_kw\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)", body)
        if not m or not m.group(1).isdigit():
            return None
        lo = int(m.group(1))
        if m.group(2).isdigit():
            params = [Param(None, "posonly", i < lo) for i in range(int(m.group(2)))]
            varargs = False
        else:
            params, varargs = [Param(None, "posonly", True) for _ in range(lo)], True
        if m.group(3) in ("false", "0"):
            return Sig(params, varargs)
        return Sig(params, varargs, open_kw=True)

    def entries(self, dict_name: str):
        table = self.dicts.get(dict_name)
        return dict(self.tables.get(table, [])) if table else {}


def stub_sig(fn: ast.FunctionDef, method: bool) -> Sig:
    a = fn.args
    positional = a.posonlyargs + a.args
    defaults = [None] * (len(positional) - len(a.defaults)) + list(a.defaults)
    n_posonly = len(a.posonlyargs)
    static = any(getattr(d, "id", "") == "staticmethod" for d in fn.decorator_list)
    if method and not static and positional:
        positional, defaults, n_posonly = positional[1:], defaults[1:], max(n_posonly - 1, 0)
    params = [Param(arg.arg, "posonly" if i < n_posonly else "pos", d is None, _is_none(d))
              for i, (arg, d) in enumerate(zip(positional, defaults))]
    params += [Param(arg.arg, "kwonly", d is None, _is_none(d))
               for arg, d in zip(a.kwonlyargs, a.kw_defaults)]
    return Sig(params, varargs=a.vararg is not None)


def _is_none(node):
    if node is None or (isinstance(node, ast.Constant) and node.value is Ellipsis):
        return None
    return isinstance(node, ast.Constant) and node.value is None


def stub_members(body) -> dict:
    """Map each name a stub body defines to its FunctionDef or ClassDef, or None."""
    out = {}
    for node in body:
        if isinstance(node, ast.FunctionDef):
            deco = {getattr(d, "id", getattr(d, "attr", "")) for d in node.decorator_list}
            if deco & {"property", "setter", "deleter"}:
                out[node.name] = None
            elif "overload" in deco:
                out.setdefault(node.name, node)
            else:
                out[node.name] = node
        elif isinstance(node, ast.ClassDef):
            out[node.name] = node
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            out[node.target.id] = None
        elif isinstance(node, ast.Assign):
            out.update((t.id, None) for t in node.targets if isinstance(t, ast.Name))
    return out


def compare(c: Sig, s: Sig) -> list[str]:
    problems = []
    if c.open_kw:
        stub_positional = [p for p in s.params if p.kind != "kwonly"]
        if len(stub_positional) != len(c.params):
            problems.append("MicroPython takes %d arguments by position and the rest by keyword only, "
                            "the stub %d by position" % (len(c.params), len(stub_positional)))
        s = Sig(stub_positional, s.varargs)
    if c.varargs != s.varargs:
        problems.append("*args on %s only" % ("MicroPython" if c.varargs else "the stub"))
    if len(c.params) != len(s.params):
        problems.append("%d arguments on MicroPython, %d in the stub" % (len(c.params), len(s.params)))
    for i, (cp, sp) in enumerate(zip(c.params, s.params)):
        label = sp.name or cp.name or "argument %d" % (i + 1)
        if cp.kind == "posonly" and sp.kind != "posonly":
            problems.append("%s: positional only on MicroPython, the stub takes it by name" % label)
        elif cp.kind != "posonly" and sp.kind == "posonly":
            problems.append("%s: positional only in the stub, MicroPython takes it by name" % label)
        elif cp.kind != sp.kind:
            problems.append("%s: keyword-only on %s only" % (
                label, "MicroPython" if cp.kind == "kwonly" else "the stub"))
        elif cp.name and sp.name and cp.name != sp.name and cp.kind != "posonly":
            problems.append("argument %d is %s on MicroPython and %s in the stub" % (i + 1, cp.name, sp.name))
        if cp.required != sp.required:
            problems.append("%s: required on %s only" % (label, "MicroPython" if cp.required else "the stub"))
        elif (not cp.required and cp.default_none is not None and sp.default_none is not None
                and cp.default_none != sp.default_none):
            problems.append("%s: the default is None on %s only" % (
                label, "MicroPython" if cp.default_none else "the stub"))
    return problems


def _ptr(value: str) -> str:
    return value.strip().replace("MP_ROM_PTR", "").strip("()& ")


def check_module(cs, mod, stub, findings, unchecked):
    stub_top = {k: v for k, v in stub_members(ast.parse(stub.read_text()).body).items()
                if not k.startswith("_")}
    c_top = {k: v for k, v in cs.entries(cs.modules[mod]).items() if not k.startswith("__")}
    for name in sorted(set(c_top) - set(stub_top)):
        findings[mod + "." + name] = ["on MicroPython, not in the stub"]
    for name in sorted(set(stub_top) - set(c_top)):
        findings[mod + "." + name] = ["in the stub, not on MicroPython"]
    for name in sorted(set(c_top) & set(stub_top)):
        node, full = stub_top[name], mod + "." + name
        if isinstance(node, ast.ClassDef):
            check_class(cs, full, _ptr(c_top[name]), node, findings, unchecked)
        elif isinstance(node, ast.FunctionDef):
            sig = cs.signature(_ptr(c_top[name]), method=False)
            if sig is None:
                unchecked.append(full)
            elif compare(sig, stub_sig(node, method=False)):
                findings[full] = compare(sig, stub_sig(node, method=False))


def check_class(cs, full, type_obj, node, findings, unchecked):
    info = cs.types.get(type_obj)
    if info is None:
        unchecked.append(full)
        return
    keep = ("__init__", "__enter__", "__exit__")
    stub = {k: v for k, v in stub_members(node.body).items() if not k.startswith("_") or k in keep}
    c = {k: v for k, v in cs.entries(info.get("locals_dict", "")).items()
         if not k.startswith("_") or k in keep}
    if "make_new" in info:
        c["__init__"] = None  # MicroPython's make_new stands for __init__
    for name in sorted(set(c) - set(stub)):
        if name == "__init__":
            continue  # a stub with no __init__ takes no arguments: checked below
        findings[full + "." + name] = ["on MicroPython, not in the stub"]
    if "protocol" in info:
        # The audiosample protocol answers these three through the type's attr.
        c.update(dict.fromkeys(("sample_rate", "bits_per_sample", "channel_count")))
    custom_attr = info.get("attr") not in (None, "cp_compat_attr")
    for name in sorted(set(stub) - set(c)):
        if custom_attr and not isinstance(stub[name], ast.FunctionDef):
            unchecked.append(full + "." + name)  # answered by the type's own attr
        elif name == "__init__":
            findings[full + ".__init__"] = ["the stub has a constructor, MicroPython can't make one"]
        elif name not in IMPLICIT:
            findings[full + "." + name] = ["in the stub, not on MicroPython"]
    for name in sorted(set(c) & set(stub)):
        sub, snode = full + "." + name, stub[name]
        if not isinstance(snode, ast.FunctionDef):
            continue
        if name == "__init__":
            sig = cs.make_new(info["make_new"])
        elif _ptr(c[name]) in cs.props:
            findings[sub] = ["a property on MicroPython, a method in the stub"]
            continue
        else:
            sig = cs.signature(_ptr(c[name]), method=True)
        if sig is None:
            unchecked.append(sub)
            continue
        problems = compare(sig, stub_sig(snode, method=True))
        if problems:
            findings[sub] = problems


def load_allowlist() -> list[re.Pattern]:
    if not ALLOWLIST.exists():
        return []
    lines = [ln.strip() for ln in ALLOWLIST.read_text().splitlines()]
    return [re.compile(ln) for ln in lines if ln and not ln.startswith("#")]


def main(argv: list[str]) -> int:
    verbose = "-v" in argv
    wanted = [a for a in argv if not a.startswith("-")]
    files = sorted(f for d in SRC.iterdir() if d.is_dir() and d.name not in NOT_BINDINGS
                   for f in d.glob("*.c"))
    files += [SRC / "mpaudio_modules.c"] + sorted((SRC / "cp_compat").glob("*.c"))
    cs = CSources(files)
    stubs = {p.stem: p for p in sorted(SRC.glob("*/*.pyi")) if p.parent.name not in NOT_BINDINGS}
    findings: dict[str, list[str]] = {}
    unchecked: list[str] = []
    for mod in wanted or sorted(stubs):
        if mod not in cs.modules:
            unchecked.append(mod)
        else:
            check_module(cs, mod, stubs[mod], findings, unchecked)
    allow = load_allowlist()
    used, failed = set(), 0
    for name in sorted(findings):
        hits = [p.pattern for p in allow if p.fullmatch(name)]
        used.update(hits)
        if not hits:
            failed += 1
            for problem in findings[name]:
                print("%s: %s" % (name, problem))
    if not wanted:
        for p in allow:
            if p.pattern not in used:
                failed += 1
                print("allowlist entry matches nothing: %s" % p.pattern)
    if verbose and unchecked:
        print("not checked:")
        for name in unchecked:
            print("  " + name)
    print("%d differ from their stub, %d not checked" % (failed, len(unchecked)), file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
