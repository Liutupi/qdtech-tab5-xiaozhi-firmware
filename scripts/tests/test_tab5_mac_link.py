"""The native UI calls tab5_mac symbols implemented in two local sources.

file(GLOB) without CONFIGURE_DEPENDS omits those sources from an incremental
build configured before the files existed. The CMake list must add each file
once when it is present, and must not invent a copy when the glob already
saw it or when the file is absent.
"""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "main/CMakeLists.txt"
NAMES = ("tab5_mac_state.cc", "tab5_mac_usb.cc")


def snippet() -> str:
    text = CMAKE.read_text()
    start = text.index("foreach(_tab5_mac_src IN ITEMS tab5_mac_state.cc tab5_mac_usb.cc)")
    end = text.index("endforeach()", start) + len("endforeach()")
    native = text.split("if(CONFIG_QDTECH_TAB5_NATIVE_UI)", 1)[1]
    excluded = native.split("else()", 1)[1].split("endif()", 1)[0]
    for name in NAMES:
        if name in excluded:
            raise AssertionError(f"{name} is filtered out of the native build")
    return text[start:end]


def run(seed: list[str], present: list[str]) -> list[str]:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        board = root / "boards/qdtech/tab5"
        board.mkdir(parents=True)
        for name in present:
            (board / name).write_text("void unused() {}\n")
        seeded = [(board / name).as_posix() for name in seed]
        listed = " ".join(f'"{path}"' for path in seeded)
        script = root / "check.cmake"
        script.write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            f'set(CMAKE_CURRENT_SOURCE_DIR "{root.as_posix()}")\n'
            f"set(BOARD_SOURCES {listed})\n"
            + snippet()
            + "\nforeach(_src IN LISTS BOARD_SOURCES)\n"
            '  message("SRC ${_src}")\n'
            "endforeach()\n"
        )
        out = subprocess.run(
            ["cmake", "-P", str(script)], check=True, capture_output=True, text=True
        )
        lines = out.stdout.splitlines() + out.stderr.splitlines()
        return [line.split("SRC ", 1)[1] for line in lines if line.startswith("SRC ")]


class Tab5MacLinkTests(unittest.TestCase):
    def test_stale_glob_gains_each_existing_source_once(self):
        added = run([], list(NAMES))
        self.assertEqual(sorted(Path(p).name for p in added), sorted(NAMES))
        self.assertEqual(len(added), len(set(added)))

    def test_sources_already_globbed_are_not_duplicated(self):
        added = run(list(NAMES), list(NAMES))
        self.assertEqual(sorted(Path(p).name for p in added), sorted(NAMES))
        self.assertEqual(len(added), len(NAMES))

    def test_missing_files_are_not_invented(self):
        kept = run(["radio_service.cc"], ["radio_service.cc"])
        self.assertEqual([Path(p).name for p in kept], ["radio_service.cc"])


if __name__ == "__main__":
    unittest.main()
