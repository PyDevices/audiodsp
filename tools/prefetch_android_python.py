"""Download cibuildwheel's Android Python tarballs ahead of the build, with retries.

cibuildwheel downloads the target Python for each Android wheel once per
build. Its own download retries a server error but gives up at once on a 4xx,
and Maven Central has answered a file that exists with a passing 404: one such
blip failed a whole release. This runs as the Android ``before-all``
(pyproject.toml), which cibuildwheel runs before any of those downloads, and
puts each tarball in cibuildwheel's cache, where its own step finds it and
downloads nothing.

It asks the cibuildwheel that is running for the URLs and checksums, so a new
cibuildwheel brings its own list. It never fails the build: if it cannot find
cibuildwheel, or a file stays unavailable, it says so and leaves the download
to cibuildwheel, as before.

    python3 tools/prefetch_android_python.py [--attempts N] [--delay SECONDS]
"""

import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

# Run inside cibuildwheel's own interpreter: the Android configurations it will
# build from, and the cache directory it looks in.
_QUERY = r"""
import json
from cibuildwheel.platforms import android
from cibuildwheel.util.file import CIBW_CACHE_PATH
print(json.dumps({
    "cache": str(CIBW_CACHE_PATH),
    "configs": [
        {"identifier": c.identifier, "url": c.url, "sha256": c.sha256}
        for c in android.all_python_configurations()
    ],
}))
"""


def log(message):
    print(f"prefetch_android_python: {message}", flush=True)


def _ancestor_pythons():
    """The interpreters of this process's ancestors (Linux only).

    cibuildwheel runs before-all through a shell, so the cibuildwheel process
    is a parent or grandparent; its argv[0] is its venv's python when it was
    started from its console script.
    """
    pid = os.getppid()
    for _ in range(6):
        if pid <= 1:
            return
        try:
            argv = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
            status = Path(f"/proc/{pid}/status").read_text()
        except OSError:
            return
        if argv and argv[0]:
            first = os.fsdecode(argv[0])
            if "python" in os.path.basename(first):
                yield first
            elif len(argv) > 1 and argv[1]:
                # A console script started by name: its shebang names the venv.
                try:
                    with open(first, "rb") as f:
                        line = f.readline().decode(errors="replace")
                    if line.startswith("#!"):
                        yield line[2:].strip().split()[0]
                except OSError:
                    pass
        match = re.search(r"^PPid:\s*(\d+)", status, re.MULTILINE)
        if not match:
            return
        pid = int(match.group(1))


def candidate_pythons():
    if os.environ.get("PREFETCH_CIBW_PYTHON"):
        yield os.environ["PREFETCH_CIBW_PYTHON"]
    if os.environ.get("RUNNER_TEMP"):
        # Where pypa/cibuildwheel's action makes its venv.
        yield str(Path(os.environ["RUNNER_TEMP"]) / "cibw" / "bin" / "python")
    yield from _ancestor_pythons()


def query_cibuildwheel():
    tried = []
    for python in candidate_pythons():
        if python in tried or not Path(python).exists():
            continue
        tried.append(python)
        result = subprocess.run(
            [python, "-c", _QUERY], capture_output=True, text=True, check=False
        )
        if result.returncode == 0:
            return python, json.loads(result.stdout)
    return None, None


def requires_python_floor(pyproject):
    try:
        text = Path(pyproject).read_text()
    except OSError:
        return None
    match = re.search(r'^requires-python\s*=\s*"\s*>=\s*3\.(\d+)', text, re.MULTILINE)
    return int(match.group(1)) if match else None


def wanted(config, floor):
    """The configurations this build will use.

    A pre-release (an "rc", "a" or "b" in the tarball's version) is built only
    when enabled, which this repository does not do; CIBW_BUILD and CIBW_SKIP
    narrow the rest as they narrow cibuildwheel.
    """
    identifier = config["identifier"]
    name = config["url"].rpartition("/")[-1]
    if re.search(r"python-\d+\.\d+\.\d+(a|b|rc)\d", name):
        return False
    match = re.match(r"cp3(\d+)-", identifier)
    if floor is not None and match and int(match.group(1)) < floor:
        return False
    build = os.environ.get("CIBW_BUILD", "").split()
    skip = os.environ.get("CIBW_SKIP", "").split()
    if build and not any(fnmatch.fnmatch(identifier, b) for b in build):
        return False
    return not any(fnmatch.fnmatch(identifier, s) for s in skip)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(url, dest, sha256, attempts, delay):
    """Download url to dest, retrying any failure, 404 included.

    Returns True when dest holds the file with the expected checksum.
    """
    if dest.exists() and (not sha256 or sha256_of(dest) == sha256):
        log(f"cached: {dest.name}")
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_name(dest.name + ".prefetch")
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                partial.write_bytes(response.read())
            if sha256 and sha256_of(partial) != sha256:
                raise ValueError("checksum does not match")
            os.replace(partial, dest)
            log(f"downloaded {dest.name} (attempt {attempt})")
            return True
        except (OSError, ValueError, urllib.error.URLError) as error:
            partial.unlink(missing_ok=True)
            log(f"attempt {attempt} of {attempts} for {url}: {error}")
            if attempt < attempts:
                time.sleep(delay * 2 ** (attempt - 1))
    return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--attempts", type=int, default=6)
    parser.add_argument("--delay", type=float, default=5.0,
                        help="first wait between attempts; it doubles each time")
    parser.add_argument("--pyproject", default="pyproject.toml")
    args = parser.parse_args(argv)

    python, info = query_cibuildwheel()
    if info is None:
        log("cibuildwheel's interpreter not found; leaving the downloads to cibuildwheel")
        return 0
    floor = requires_python_floor(args.pyproject)
    configs = [c for c in info["configs"] if wanted(c, floor)]
    cache = Path(info["cache"])
    log(f"{len(configs)} target Pythons from {python}, into {cache}")

    missing = []
    seen = set()
    for config in configs:
        name = config["url"].rpartition("/")[-1]
        if name in seen:
            continue
        seen.add(name)
        if not fetch(config["url"], cache / name, config.get("sha256"),
                     args.attempts, args.delay):
            missing.append(name)
    if missing:
        log("still unavailable, leaving to cibuildwheel: " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
