#!/usr/bin/env python3
"""Fail if a ``deinit()`` does work outside the pump lock.

The live-audio-path audit's rule, in one line: **a deinit() needs the lock
over its whole body.** Not over the part someone argued was the dangerous
one -- over all of it.

The reason is that ``audiosample_mark_deinit`` is not the dangerous part. The
damage is in the pointer-nulling that follows it: ``storage`` is the sole GC
root for eight interior pointers a pull still holds, a ``reset()`` writes the
running state a pull reads, an idempotency flag is the same word a second
``deinit()`` reads. Each of those was argued safe -- "the funnel refuses a
pull once the object is marked", "only the interpreter thread calls
deinit()" -- and an argument is not a guarantee. audiodsp#116.

So the shape is checked instead of remembered. A ``deinit()`` that takes the
lock at all must take it before its first statement and release it after its
last, with nothing between the function's opening brace and the acquire
except the ``self`` cast, and nothing between the release and the closing
brace except the return.

Run it::

    python3 tools/check_lock_shape.py              # the whole repo
    python3 tools/check_lock_shape.py --self-test  # prove it can fail
    python3 tools/check_lock_shape.py src/audioroute

Exit status is 0 when clean, 1 when a body does work outside its lock.

What it does not check, deliberately: whether a ``deinit()`` takes the lock
at all. Several node types have nothing a pull can see -- no source, no
borrowed pointer, no running state -- and giving them a lock they do not need
would be noise. What the rule catches is the body that takes the lock and
then reaches past it, which is the shape every one of the audit's findings
had.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

ROOTS = ("src",)

ACQUIRE = "audiodsp_pump_lock_acquire"
RELEASE = "audiodsp_pump_lock_release"

#: A C function whose name ends in `_deinit`. `_release` bodies (the CPython
#: target's) are not on this path: the extension has no pump lock.
#:
#: `check_for_deinit` is excluded by name, and the exclusion is the
#: interesting part. Those helpers read one word under the lock and **raise
#: outside it on purpose**: the lock's contract is that nothing which can
#: longjmp runs while it is held, because a raise never reaches the release
#: and the pump then blocks on a mutex owned by a thread that has gone back
#: to the interpreter. They are the opposite rule, not a violation of this
#: one, and a checker that did not know the difference would report
#: fourteen of them and be switched off.
DEINIT = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_ *]*\b((?!check_for_deinit)\w*_deinit)"
    r"\s*\([^;{]*\)\s*\{",
    re.MULTILINE)

#: What may stand between the opening brace and the acquire: the cast that
#: gets `self` out of the object pointer, and nothing else. A declaration
#: with an initialiser that reads the object is a read outside the lock.
PROLOGUE_OK = re.compile(
    r"^\s*(?:"
    r"[A-Za-z_][A-Za-z0-9_]*\s*\*\s*\w+\s*=\s*MP_OBJ_TO_PTR\s*\([^;]*\)\s*;"
    r"|//.*|/\*.*\*/|"
    r")\s*$")

#: What may follow the release: the return, and nothing else.
EPILOGUE_OK = re.compile(r"^\s*(?:return\b[^;]*;|//.*|/\*.*\*/|)\s*$")


def strip_comments(text: str) -> str:
    """Blank comments, keeping line count and offsets."""
    out = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            out.append("".join(
                character if character == "\n" else " "
                for character in text[index:end]))
            index = end
        elif text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end < 0 else end
            out.append(" " * (end - index))
            index = end
        elif text[index] in "\"'":
            quote = text[index]
            end = index + 1
            while end < length and text[end] != quote:
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
            out.append(" " * (end - index))
            index = end
        else:
            out.append(text[index])
            index += 1
    return "".join(out)


def body_of(text: str, brace: int) -> tuple[str, int]:
    """The function body at `brace` (the index of its `{`), and its end."""
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:index], index
    return text[brace + 1:], len(text)


def check_text(source: str, path: str) -> list[str]:
    blanked = strip_comments(source)
    problems = []
    for match in DEINIT.finditer(blanked):
        name = match.group(1)
        body, _end = body_of(blanked, match.end() - 1)
        if ACQUIRE not in body:
            continue
        line = source.count("\n", 0, match.start()) + 1
        where = "%s:%d %s()" % (path, line, name)

        first = body.index(ACQUIRE)
        last = body.rindex(RELEASE)
        if last < first:
            problems.append("%s: releases before it acquires" % where)
            continue

        prologue = body[:body.rfind("\n", 0, first) + 1]
        for offset, text in enumerate(prologue.splitlines()):
            if not PROLOGUE_OK.match(text):
                problems.append(
                    "%s: line %d runs before the lock is taken: %s"
                    % (where, line + offset + 1, text.strip()))

        epilogue = body[body.find("\n", last) + 1:]
        for offset, text in enumerate(epilogue.splitlines()):
            if not EPILOGUE_OK.match(text):
                problems.append(
                    "%s: a statement runs after the lock is released: %s"
                    % (where, text.strip()))

        # An early return between the acquire and the release leaves the lock
        # held. `continue`/`break` cannot: there is no loop around the whole
        # body of any of these.
        held = body[first:last]
        for offset, text in enumerate(held.splitlines()):
            if re.search(r"\breturn\b", text) and RELEASE not in text:
                previous = held.splitlines()[max(0, offset - 1)]
                if RELEASE not in previous:
                    problems.append(
                        "%s: returns with the lock held: %s"
                        % (where, text.strip()))
    return problems


SELF_TEST = {
    "a statement after the release": """
static mp_obj_t node_deinit(mp_obj_t self_in) {
    node_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    self->source = mp_const_none;
    audiodsp_pump_lock_release();
    self->storage = NULL;
    return mp_const_none;
}
""",
    "a statement before the acquire": """
static mp_obj_t node_deinit(mp_obj_t self_in) {
    node_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->deinited) {
        return mp_const_none;
    }
    audiodsp_pump_lock_acquire();
    self->source = mp_const_none;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
""",
    "a return with the lock held": """
static mp_obj_t node_deinit(mp_obj_t self_in) {
    node_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    if (self->deinited) {
        return mp_const_none;
    }
    audiodsp_pump_lock_release();
    return mp_const_none;
}
""",
    "a release before its acquire": """
static mp_obj_t node_deinit(mp_obj_t self_in) {
    node_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_release();
    self->source = mp_const_none;
    audiodsp_pump_lock_acquire();
    return mp_const_none;
}
""",
}

CLEAN = """
static void check_for_deinit(node_obj_t *self) {
    audiodsp_pump_lock_acquire();
    const bool released = audiosample_deinited(&self->base);
    audiodsp_pump_lock_release();
    if (released) {
        audiosample_check_for_deinit(&self->base);
    }
}

static mp_obj_t node_deinit(mp_obj_t self_in) {
    node_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // A comment before the lock is not a statement.
    audiodsp_pump_lock_acquire();
    if (self->deinited) {
        audiodsp_pump_lock_release();
        return mp_const_none;
    }
    self->source = mp_const_none;
    audiodsp_pump_lock_release();
    return mp_const_none;
}

static mp_obj_t other_deinit(mp_obj_t self_in) {
    other_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->nothing_a_pull_can_see = 0;
    return mp_const_none;
}
"""


def self_test() -> int:
    """A checker built out of regexes is one bad pattern away from matching
    nothing and reporting a clean tree for ever. Every shape it exists to
    catch is planted here, and the clean one is planted beside them so a
    checker that simply says "no" to everything fails too."""
    failures = 0
    for name, text in SELF_TEST.items():
        found = check_text(text, "<self-test>")
        if not found:
            print("SELF-TEST FAILED: %s was not caught" % name)
            failures += 1
        else:
            print("caught: %s -- %s" % (name, found[0].split(": ", 1)[1]))
    found = check_text(CLEAN, "<self-test>")
    if found:
        print("SELF-TEST FAILED: the clean shape was reported: %r" % (found,))
        failures += 1
    else:
        print("clean: a body that holds the lock over all of itself passes, "
              "a body with no lock is left alone, and check_for_deinit's "
              "deliberate raise-outside-the-lock is not reported")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="*", help="subtrees to check")
    parser.add_argument("--self-test", action="store_true",
                        help="plant every banned shape and require a catch")
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        return self_test()

    roots = ([Path(path) for path in arguments.paths]
             or [REPO / root for root in ROOTS])
    problems = []
    checked = 0
    for root in roots:
        for path in sorted(root.rglob("*.c")):
            checked += 1
            problems.extend(check_text(
                path.read_text(encoding="utf-8", errors="replace"),
                str(path.relative_to(REPO) if path.is_absolute() else path)))
    if problems:
        for problem in problems:
            print(problem)
        print("\n%d deinit body/bodies do work outside the pump lock."
              % len(problems))
        return 1
    print("%d C files: every deinit() that takes the pump lock holds it over "
          "its whole body." % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
