#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 PyDevices contributors
# SPDX-License-Identifier: MIT
"""Say which CI jobs a change can reach.

A pull request is diffed against its base, a push to main from the commit main
was at before the push to the one it moved to. Each changed file names the
jobs it can reach in RULES; a file no rule names, and any case with no usable
base (a manual or scheduled run, an unreachable commit), reaches every job.

Jobs: lint (lint.yml), unit (test-cpython.yml's matrix), wheels (the
cibuildwheel and [render] jobs there) and build (clean-build.yml).

    ci_scope.py --event pull_request --base <sha> [--head HEAD]
    ci_scope.py --files tools/check_portable.py tests/pump/pump_probe.py

Prints one line per job and, under GitHub Actions, writes the same to
$GITHUB_OUTPUT. `--self-test` is not needed: tests/test_ci_scope.py holds it.
"""

import argparse
import os
import re
import subprocess
import sys

JOBS = ("lint", "unit", "wheels", "build")
ALL = frozenset(JOBS)

# First match wins. A pattern is a glob over the path from the repo root:
# `*` stays inside one directory, `**` crosses them. Anything that matches no pattern reaches
# every job: src/, third_party/, setup.py, pyproject.toml, MANIFEST.in, the
# micropython and cmake build files, the pins and every file new to the repo.
RULES = (
    # Nothing reads these. The workflows already skip md, docs and LICENSE.
    ("**.md", ()),
    ("docs/**", ()),
    ("LICENSE", ()),
    # VERSION reaches the wheel's version string and the firmware's __version__,
    # which every test takes from the file and none compares with a number.
    ("VERSION", ()),
    # Other workflows: they publish, tag and prepare releases, and run on their
    # own events. Each of the three CI workflows reaches only its own jobs.
    (".github/workflows/lint.yml", ("lint",)),
    (".github/workflows/test-cpython.yml", ("unit", "wheels")),
    (".github/workflows/clean-build.yml", ("build",)),
    (".github/workflows/android-wheels.yml", ()),
    (".github/workflows/prepare-release.yml", ()),
    (".github/workflows/publish-release-packages.yml", ()),
    (".github/workflows/tag-release.yml", ()),
    # flake8 reads the examples; nothing else does.
    ("examples/**", ("lint",)),
    (".flake8", ("lint",)),
    ("scripts/check_attribution.py", ("lint",)),
    ("scripts/test_testpypi_install.py", ("lint",)),
    ("tools/check_*.py", ("lint",)),
    ("tools/design_*.py", ("lint",)),
    ("tools/prefetch_android_python.py", ("lint", "unit")),
    # Probes, pump gates and CircuitPython's mixer check run on an interpreter
    # build and, for the first two, on CPython; none of them goes into a wheel.
    ("tests/circuitpython/**", ("lint", "build")),
    ("tests/pump/**", ("lint", "unit", "build")),
    ("tests/parity/**", ("lint", "unit", "build")),
    ("tests/data/**", ("lint", "unit", "build")),
    ("tests/cmake/**", ("build",)),
    ("tests/test_*.py", ("lint", "unit")),
    ("tests/support/**", ("lint", "unit")),
    # This tool and its own test.
    ("tools/ci_scope.py", tuple(JOBS)),
)


def compile_glob(pattern):
    return re.compile(
        "".join(".*" if p == "**" else "[^/]*" if p == "*" else re.escape(p)
                for p in re.split(r"(\*\*|\*)", pattern)) + r"\Z")


COMPILED = [(compile_glob(p), j) for p, j in RULES]


def reach(path):
    for pattern, jobs in COMPILED:
        if pattern.match(path):
            return frozenset(jobs)
    return ALL


def scope(files):
    """The jobs a set of changed files can reach, and why each was kept."""
    jobs = set()
    for path in files:
        jobs |= reach(path)
    return jobs


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def changed_files(base, head):
    """The files that differ between base and head, or None if git can't say."""
    if not base or re.fullmatch(r"0+", base):
        return None
    try:
        git("cat-file", "-e", base + "^{commit}")
    except subprocess.CalledProcessError:
        try:
            git("fetch", "--no-tags", "--depth=1", "origin", base)
        except subprocess.CalledProcessError:
            return None
    try:
        return git("diff", "--name-only", "--no-renames", base, head).split("\n")[:-1]
    except subprocess.CalledProcessError:
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--event", default="", help="GitHub's event name; only pull_request and push are scoped")
    ap.add_argument("--base", default="", help="the commit to diff from")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--files", nargs="*", help="changed files, instead of a diff")
    args = ap.parse_args(argv)

    if args.files is not None:
        files, why = args.files, "the files given"
    elif args.event not in ("pull_request", "push"):
        files, why = None, "event %r is not a change" % args.event
    else:
        files = changed_files(args.base, args.head)
        why = "%s..%s" % (args.base[:9], args.head)
        if files is None:
            why = "no usable base %r" % args.base

    if files is None:
        jobs = set(ALL)
        print("Everything runs: %s." % why)
    else:
        jobs = scope(files)
        print("Scope (%s): %d changed files." % (why, len(files)))
        for path in files:
            print("  %-60s %s" % (path, ", ".join(sorted(reach(path))) or "-"))
    for job in JOBS:
        print("%s=%s" % (job, "true" if job in jobs else "false"))
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as f:
            for job in JOBS:
                f.write("%s=%s\n" % (job, "true" if job in jobs else "false"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
