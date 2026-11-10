# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Sphinx configuration for the audiodsp documentation.

Everything shared by the PyDevices documentation projects (theme, top bar,
Markdown and docstring setup) comes from pydevices-docs.

The module reference is written from the type stubs, ``src/<module>/<module>.pyi``,
which list every name each native module exports. The build copies each stub
into ``_build`` as an importable module, so autodoc reads the stubs and never
imports the compiled modules.
"""

import re
import sys
from pathlib import Path

from pydevices_docs import configure

globals().update(configure(
    "audiodsp", __file__,
    repo="PyDevices/audiodsp", edit_path="docs",
    version_file="../VERSION",
))

_DOCS = Path(__file__).resolve().parent
_STUBS = _DOCS.parent / "src"
_STUB_ROOT = _DOCS / "_build" / "stubs"


def _importable(text):
    """A stub may name a class before it is defined, which a stub allows and
    Python before 3.14 does not, so defer the annotations."""
    return re.sub(r"^((?:from|import) )", r"from __future__ import annotations\n\1",
                  text, count=1, flags=re.M)


def _install_stubs(app):
    """Make every module's stub importable under the module's own name."""
    _STUB_ROOT.mkdir(parents=True, exist_ok=True)
    if str(_STUB_ROOT) not in sys.path:
        sys.path.insert(0, str(_STUB_ROOT))
    for stub in sorted(_STUBS.glob("*/*.pyi")):
        target = _STUB_ROOT / (stub.stem + ".py")
        text = _importable(stub.read_text(encoding="utf-8"))
        if not target.exists() or target.read_text(encoding="utf-8") != text:
            target.write_text(text, encoding="utf-8")
        sys.modules.pop(stub.stem, None)


def setup(app):
    app.connect("builder-inited", _install_stubs)
    return {"version": "1", "parallel_read_safe": True, "parallel_write_safe": True}
