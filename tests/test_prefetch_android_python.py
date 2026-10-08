"""tools/prefetch_android_python.py: a passing 404 no longer fails the Android build."""

import hashlib
import importlib.util
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "prefetch_android_python.py"

spec = importlib.util.spec_from_file_location("prefetch_android_python", TOOL)
prefetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prefetch)

PAYLOAD = b"not really a python tarball\n" * 100
PAYLOAD_SHA = hashlib.sha256(PAYLOAD).hexdigest()


class FlakyServer:
    """Answers 404 to the first `failures` requests, then serves PAYLOAD."""

    def __init__(self, failures):
        server = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                server.requests += 1
                if server.requests <= server.failures:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(PAYLOAD)))
                self.end_headers()
                self.wfile.write(PAYLOAD)

            def log_message(self, *args):
                pass

        self.failures = failures
        self.requests = 0
        self.httpd = HTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def url(self, name):
        return f"http://127.0.0.1:{self.httpd.server_port}/{name}"

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self.httpd.server_close()


class FetchTest(unittest.TestCase):
    def test_a_404_is_retried(self):
        with FlakyServer(failures=2) as server, tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "python.tar.gz"
            ok = prefetch.fetch(server.url("python.tar.gz"), dest, PAYLOAD_SHA, 4, 0)
            self.assertTrue(ok)
            self.assertEqual(dest.read_bytes(), PAYLOAD)
            self.assertEqual(server.requests, 3)

    def test_one_try_fails_on_the_404(self):
        # The behaviour before the retry, which failed a release: one attempt.
        with FlakyServer(failures=1) as server, tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "python.tar.gz"
            ok = prefetch.fetch(server.url("python.tar.gz"), dest, PAYLOAD_SHA, 1, 0)
            self.assertFalse(ok)
            self.assertFalse(dest.exists())

    def test_a_bad_checksum_is_not_cached(self):
        with FlakyServer(failures=0) as server, tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "python.tar.gz"
            ok = prefetch.fetch(server.url("python.tar.gz"), dest, "0" * 64, 2, 0)
            self.assertFalse(ok)
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_a_cached_file_is_not_downloaded_again(self):
        with FlakyServer(failures=0) as server, tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "python.tar.gz"
            dest.write_bytes(PAYLOAD)
            self.assertTrue(prefetch.fetch(server.url("x"), dest, PAYLOAD_SHA, 1, 0))
            self.assertEqual(server.requests, 0)


class WantedTest(unittest.TestCase):
    # cibuildwheel 4.2.1's Android list, shortened.
    CONFIGS = [
        {"identifier": "cp313-android_arm64_v8a",
         "url": "https://repo.maven.apache.org/maven2/com/chaquo/python/python/3.13.15/python-3.13.15-aarch64-linux-android.tar.gz"},
        {"identifier": "cp314-android_x86_64",
         "url": "https://www.python.org/ftp/python/3.14.7/python-3.14.7-x86_64-linux-android.tar.gz"},
        {"identifier": "cp315-android_x86_64",
         "url": "https://www.python.org/ftp/python/3.15.0/python-3.15.0rc2-x86_64-linux-android.tar.gz"},
        {"identifier": "cp312-android_x86_64",
         "url": "https://example.invalid/python-3.12.9-x86_64-linux-android.tar.gz"},
    ]

    def test_selection(self):
        env = {k: v for k, v in os.environ.items() if k not in ("CIBW_BUILD", "CIBW_SKIP")}
        saved = os.environ.copy()
        try:
            os.environ.clear()
            os.environ.update(env)
            picked = [c["identifier"] for c in self.CONFIGS if prefetch.wanted(c, 13)]
            self.assertEqual(picked, ["cp313-android_arm64_v8a", "cp314-android_x86_64"])
            os.environ["CIBW_SKIP"] = "cp313-*"
            picked = [c["identifier"] for c in self.CONFIGS if prefetch.wanted(c, 13)]
            self.assertEqual(picked, ["cp314-android_x86_64"])
        finally:
            os.environ.clear()
            os.environ.update(saved)


class EndToEndTest(unittest.TestCase):
    """main() asks a cibuildwheel for its list and fills that cibuildwheel's cache."""

    def test_fills_the_cache_through_a_404(self):
        with FlakyServer(failures=1) as server, tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "fake"
            cache = Path(tmp) / "cache"
            (fake / "cibuildwheel" / "platforms").mkdir(parents=True)
            (fake / "cibuildwheel" / "util").mkdir(parents=True)
            for d in ("cibuildwheel", "cibuildwheel/platforms", "cibuildwheel/util"):
                (fake / d / "__init__.py").write_text("")
            (fake / "cibuildwheel" / "util" / "file.py").write_text(
                f"from pathlib import Path\nCIBW_CACHE_PATH = Path({str(cache)!r})\n")
            (fake / "cibuildwheel" / "platforms" / "android.py").write_text(
                "class C:\n"
                "    def __init__(self, **kw): self.__dict__.update(kw)\n"
                "def all_python_configurations():\n"
                f"    return [C(identifier='cp313-android_x86_64',"
                f" url={server.url('python-3.13.15-x86_64-linux-android.tar.gz')!r},"
                f" sha256={PAYLOAD_SHA!r})]\n")
            env = dict(os.environ, PREFETCH_CIBW_PYTHON=sys.executable, PYTHONPATH=str(fake))
            env.pop("CIBW_BUILD", None)
            env.pop("CIBW_SKIP", None)
            result = subprocess.run(
                [sys.executable, str(TOOL), "--delay", "0",
                 "--pyproject", str(ROOT / "pyproject.toml")],
                env=env, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            landed = cache / "python-3.13.15-x86_64-linux-android.tar.gz"
            self.assertEqual(landed.read_bytes(), PAYLOAD, result.stdout)
            self.assertEqual(server.requests, 2)

    def test_no_cibuildwheel_is_not_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            # Shadow any real cibuildwheel, so nothing is downloaded.
            (Path(tmp) / "cibuildwheel").mkdir()
            (Path(tmp) / "cibuildwheel" / "__init__.py").write_text("raise ImportError\n")
            env = dict(os.environ, PREFETCH_CIBW_PYTHON=sys.executable, PYTHONPATH=tmp)
            env.pop("RUNNER_TEMP", None)
            result = subprocess.run([sys.executable, str(TOOL)], env=env,
                                    capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("not found", result.stdout)


if __name__ == "__main__":
    unittest.main()
