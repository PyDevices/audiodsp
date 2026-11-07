# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""audiomp3 on CPython decodes the same bytes a MicroPython build does.

The hash below is the unix MicroPython build's decode of
tests/parity/fixtures/rewind.mp3: every buffer `audiocore.get_buffer` hands
back, the last one included, until the result isn't MORE_DATA. The decoder
here is the same Helix C, so the bytes must match exactly, on every platform
a wheel is built for.
"""

import hashlib
import io
import os
import pathlib
import tempfile
import unittest

import audiocore
import audiomp3

FIXTURE = pathlib.Path(__file__).resolve().parent / "parity" / "fixtures" / "rewind.mp3"
#: From MicroPython's unix build, decoding FIXTURE with the loop in `decode`.
MICROPYTHON_SHA256 = "91a7cd4d400112b07d627da8f3f101686e176f51ef3cf7ffd8804d9bef87b33c"
MICROPYTHON_BYTES = 539136
MICROPYTHON_BUFFERS = 117


def decode(decoder):
    """Every buffer's bytes, the result that ended the lap, and the count."""
    data = bytearray()
    buffers = 0
    while True:
        result, buffer = audiocore.get_buffer(decoder)
        data += buffer
        buffers += 1
        if result != audiocore.GET_BUFFER_MORE_DATA:
            return bytes(data), result, buffers


class DecodesLikeMicroPython(unittest.TestCase):

    def test_the_bytes_match(self):
        with audiomp3.MP3Decoder(str(FIXTURE)) as decoder:
            data, result, buffers = decode(decoder)
            self.assertEqual(result, audiocore.GET_BUFFER_DONE)
            self.assertEqual(buffers, MICROPYTHON_BUFFERS)
            self.assertEqual(len(data), MICROPYTHON_BYTES)
            self.assertEqual(hashlib.sha256(data).hexdigest(), MICROPYTHON_SHA256)
            self.assertEqual(decoder.samples_decoded, MICROPYTHON_BYTES // 2)

    def test_the_properties(self):
        with open(FIXTURE, "rb") as handle:
            decoder = audiomp3.MP3Decoder(handle)
            self.assertIs(decoder.file, handle)
            self.assertEqual(decoder.sample_rate, 44100)
            self.assertEqual(decoder.channel_count, 2)
            self.assertEqual(decoder.bits_per_sample, 16)
            self.assertEqual(decoder.samples_decoded, 0)
            self.assertEqual(decoder.rms_level, 0.0)
            audiocore.get_buffer(decoder)
            self.assertEqual(decoder.samples_decoded, 2304)
            structure = audiocore.get_structure(decoder)
            self.assertEqual(structure["sample_rate"], 44100)
            decoder.deinit()
            with self.assertRaises(ValueError):
                decoder.sample_rate

    def test_a_rewound_lap_matches_the_first(self):
        with audiomp3.MP3Decoder(str(FIXTURE)) as decoder:
            first = decode(decoder)
            audiocore.reset_buffer(decoder)
            self.assertEqual(decoder.samples_decoded, 0)
            self.assertEqual(decode(decoder), first)

    def test_a_large_buffer_decodes_the_same(self):
        buffer = bytearray(32768)
        with audiomp3.MP3Decoder(str(FIXTURE), buffer) as decoder:
            data, _, _ = decode(decoder)
        self.assertEqual(hashlib.sha256(data).hexdigest(), MICROPYTHON_SHA256)

    def test_a_stream_that_cannot_seek(self):
        """Read past the ID3 tag rather than seek over it, as on a socket."""

        class Unseekable(io.RawIOBase):
            def __init__(self, data):
                self._data = io.BytesIO(data)

            def readable(self):
                return True

            def readinto(self, buffer):
                return self._data.readinto(buffer)

        decoder = audiomp3.MP3Decoder(io.BufferedReader(Unseekable(FIXTURE.read_bytes())))
        data, result, _ = decode(decoder)
        self.assertEqual(result, audiocore.GET_BUFFER_DONE)
        self.assertEqual(hashlib.sha256(data).hexdigest(), MICROPYTHON_SHA256)

    def test_a_file_cut_mid_frame_ends_done(self):
        """CircuitPython 11's fix, as tests/parity/cp11_files_probe.py checks it."""
        whole = FIXTURE.read_bytes()
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "cut.mp3")
            with open(path, "wb") as handle:
                handle.write(whole[:len(whole) * 2 // 3 + 77])
            with audiomp3.MP3Decoder(path) as decoder:
                for _ in range(2):
                    _, result, buffers = decode(decoder)
                    self.assertEqual((result, buffers), (audiocore.GET_BUFFER_DONE, 79))
                    audiocore.reset_buffer(decoder)

    def test_open_plays_another_file(self):
        with audiomp3.MP3Decoder(str(FIXTURE)) as decoder:
            decode(decoder)
            decoder.open(str(FIXTURE))
            data, _, _ = decode(decoder)
        self.assertEqual(hashlib.sha256(data).hexdigest(), MICROPYTHON_SHA256)

    def test_refusals(self):
        with self.assertRaises(RuntimeError):
            audiomp3.MP3Decoder(io.BytesIO(bytes(4096)))
        with open(FIXTURE, "r", encoding="latin-1") as text:
            with self.assertRaises(TypeError):
                audiomp3.MP3Decoder(text)


if __name__ == "__main__":
    unittest.main()
