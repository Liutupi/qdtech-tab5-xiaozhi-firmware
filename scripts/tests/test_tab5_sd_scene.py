"""Portable parser, buffer ownership, scene transitions and audio priority."""
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
TESTS = Path(__file__).parent


def fixture(path, width, height, timestamps, duration):
    # Two distinct frames, aliased by many timeline entries. Small fixture file.
    size = width * height * 3
    frames = [bytes([value]) * size for value in [0x32, 0x91]]
    offset = 64 + len(timestamps) * 32
    index = b"".join(struct.pack("<IIIIHHHHBBHI", at, offset + (n % 2) * size,
        size, zlib.crc32(frames[n % 2]), 0, 0, width, height, 1, 1, 0, 0)
        for n, at in enumerate(timestamps))
    header = bytearray(64)
    header[:8] = b"NABOSD1\0"
    struct.pack_into("<HHHHHH", header, 8, 1, 64, width, height, width, height)
    struct.pack_into("<IIIIIIIII", header, 20, len(timestamps), duration, 64,
        offset, size, 1, offset + 2 * size, zlib.crc32(index), 0)
    struct.pack_into("<I", header, 60, zlib.crc32(header[:60]))
    path.write_bytes(header + index + b"".join(frames))


class Tab5SdSceneTests(unittest.TestCase):
    def test_runtime_contracts(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ host compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            idle = tmp / "idle.nab"
            eye = tmp / "eye.nab"
            fixture(idle, 320, 412, list(range(0, 64 * 40, 40)), 64 * 40)
            fixture(eye, 204, 93, [0, 3280, 3360, 3440, 3480], 5000)
            for name, args in [("mailbox", [idle]), ("states", []),
                               ("policy", []), ("pack", [eye]), ("trace", [idle])]:
                binary = tmp / name
                subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(BOARD), str(TESTS / f"tab5_sd_{name}_smoke.cc"),
                    "-o", str(binary)], check=True)
                subprocess.run([str(binary), *map(str, args)], check=True)


if __name__ == "__main__":
    unittest.main()
