# SPDX-FileCopyrightText: 2026 PyDevices contributors
# SPDX-License-Identifier: MIT
"""tools/ci_scope.py decides which CI jobs a change reaches; this holds its rules."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent / "tools" / "ci_scope.py"
sys.path.insert(0, str(TOOL.parent))
import ci_scope  # noqa: E402


class Rules(unittest.TestCase):
    def jobs(self, *files):
        return ci_scope.scope(files)

    def test_docs_reach_nothing(self):
        self.assertEqual(self.jobs("README.md", "docs/how-it-works/x.md", "docs/img/a.png", "LICENSE"), set())

    def test_a_release_bump_reaches_nothing(self):
        self.assertEqual(self.jobs("VERSION", "CHANGELOG.md"), set())

    def test_examples_are_only_linted(self):
        self.assertEqual(self.jobs("examples/audiodsp_simpletest.py"), {"lint"})

    def test_a_pump_probe_skips_the_wheel_jobs(self):
        self.assertEqual(self.jobs("tests/pump/pump_probe.py"), {"lint", "unit", "build"})

    def test_a_unit_test_skips_the_interpreter_build(self):
        self.assertEqual(self.jobs("tests/test_cpython_audiograph.py"), {"lint", "unit"})

    def test_a_workflow_reaches_its_own_jobs(self):
        self.assertEqual(self.jobs(".github/workflows/clean-build.yml"), {"build"})
        self.assertEqual(self.jobs(".github/workflows/lint.yml"), {"lint"})
        self.assertEqual(self.jobs(".github/workflows/prepare-release.yml"), set())

    def test_the_sources_packaging_and_unknown_files_reach_everything(self):
        for path in ("src/shared/audiodsp_graph.c", "src/cpython/_audiodsp.c", "setup.py", "pyproject.toml",
                     "MANIFEST.in", "micropython.mk", "DEPENDENCIES.lock", "third_party/tremor/x.c",
                     "a_file_no_rule_names.txt", "tools/ci_scope.py", "tests/vendor/test_x.py"):
            self.assertEqual(self.jobs(path), set(ci_scope.JOBS), path)

    def test_a_star_stays_inside_one_directory(self):
        self.assertEqual(self.jobs("tests/test_cpython_x.py"), {"lint", "unit"})
        self.assertEqual(self.jobs("tests/nested/test_x.py"), set(ci_scope.JOBS))

    def test_a_union_over_the_files(self):
        self.assertEqual(self.jobs("examples/a.py", "tests/circuitpython/ring_through_mixer.py"), {"lint", "build"})


class Command(unittest.TestCase):
    def run_tool(self, *args, env=None):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out")
            e = dict(os.environ, GITHUB_OUTPUT=out)
            r = subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, env=e,
                               cwd=str(TOOL.parent.parent))
            self.assertEqual(r.returncode, 0, r.stderr)
            return dict(line.split("=") for line in Path(out).read_text().split()), r.stdout

    def test_files_mode(self):
        got, _ = self.run_tool("--files", "VERSION", "examples/a.py")
        self.assertEqual(got, {"lint": "true", "unit": "false", "wheels": "false", "build": "false"})

    def test_a_manual_or_scheduled_run_runs_everything(self):
        for event in ("workflow_dispatch", "schedule", ""):
            got, _ = self.run_tool("--event", event, "--base", "HEAD")
            self.assertEqual(set(got.values()), {"true"}, event)

    def test_no_usable_base_runs_everything(self):
        for base in ("", "0" * 40, "f" * 40):
            got, _ = self.run_tool("--event", "push", "--base", base)
            self.assertEqual(set(got.values()), {"true"}, base)


if __name__ == "__main__":
    unittest.main()
