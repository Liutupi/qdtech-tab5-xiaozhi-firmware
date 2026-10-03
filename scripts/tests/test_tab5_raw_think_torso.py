"""Verify the opt-in raw Think torso matches the generated LZ4 pose."""

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location(
    "prepare_nabo_think_torso_raw", ROOT / "scripts/prepare_nabo_think_torso_raw.py")
raw_asset = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(raw_asset)
from nabo_image_codec import lz4_block  # noqa: E402


class Tab5RawThinkTorsoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = raw_asset.OUTPUT.read_bytes()

    def test_source_pixels_are_identical_to_compressed_pose(self):
        self.assertEqual(self.data, raw_asset.prepare())
        self.assertEqual(len(self.data), raw_asset.WIDTH * raw_asset.HEIGHT * 3)
        compressed = lz4_block(self.data)
        self.assertEqual(raw_asset.embedded_lz4(),
                         struct.pack("<III", 2, len(compressed), len(self.data)) + compressed)
        # Alpha is stored after the full RGB565 plane, as LVGL RGB565A8 expects.
        alpha = self.data[raw_asset.WIDTH * raw_asset.HEIGHT * 2:]
        self.assertEqual(len(alpha), raw_asset.WIDTH * raw_asset.HEIGHT)
        self.assertIn(0, alpha)
        self.assertIn(255, alpha)

    def test_disabled_native_variant_has_no_raw_resource(self):
        config = json.loads((ROOT / "main/boards/qdtech/tab5/config.json").read_text())
        native = next(build for build in config["builds"]
                      if build["name"] == "qdtech-tab5-native")
        self.assertIn("CONFIG_QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT=n",
                      native["sdkconfig_append"])
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        self.assertRegex(kconfig,
                         r"config QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT\n(?:.*\n)*?    default n")
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn('list(FILTER BOARD_SOURCES EXCLUDE REGEX "/nabo_think_torso_raw', cmake)
        self.assertIn("if(CONFIG_QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT)", cmake)
        display = (ROOT / "main/boards/qdtech/tab5/tab5_native_display.h").read_text()
        branch = display.split("const lv_image_dsc_t* torso = rig.torso;", 1)[1].split(
            "bind(wave_torso_, torso, pose_geometry_.torso);", 1)[0]
        for defines, raw_selected in (([], False),
                                      (["-DCONFIG_QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT=1"],
                                       True)):
            result = subprocess.run(["cc", "-E", "-P", "-x", "c++", *defines, "-"],
                                    input=branch, capture_output=True, text=True, check=True)
            self.assertEqual("&nabo_think_torso_raw" in result.stdout, raw_selected)
            self.assertEqual("nabo_think_torso_raw_available()" in result.stdout, raw_selected)

    def test_cmake_rejects_same_length_corruption(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        enabled = cmake.split("if(CONFIG_QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT)", 1)[1].split(
            "else()\n", 1)[0]
        validation = enabled.split('file(SIZE "${TAB5_THINK_TORSO_RAW_BIN}"', 1)[1].split(
            "set(TAB5_THINK_TORSO_RAW_ASM", 1)[0]
        validation = 'file(SIZE "${TAB5_THINK_TORSO_RAW_BIN}"' + validation
        self.assertIn("CMAKE_CONFIGURE_DEPENDS", enabled)
        self.assertIn("OBJECT_DEPENDS ${TAB5_THINK_TORSO_RAW_BIN}", enabled)
        self.assertIn(hashlib.sha256(self.data).hexdigest(), validation)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            candidate = directory / "think.bin"
            script = directory / "validate.cmake"
            script.write_text(f'set(TAB5_THINK_TORSO_RAW_BIN "{candidate}")\n{validation}\n')
            for data, accepted in ((self.data, True),
                                   (bytes([self.data[0] ^ 1]) + self.data[1:], False),
                                   (self.data[:-1], False)):
                candidate.write_bytes(data)
                result = subprocess.run(["cmake", "-P", str(script)], capture_output=True,
                                        text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)

    def test_assembled_resource_has_alignment_and_exact_bytes(self):
        compiler = sorted((Path.home() / ".espressif/tools/riscv32-esp-elf").glob(
            "*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"))
        if not compiler:
            self.skipTest("ESP RISC-V assembler is not installed")
        compiler = compiler[-1]
        template = (ROOT / "main/boards/qdtech/tab5/nabo_think_torso_raw_embedded.S.in").read_text()
        self.assertIn(".balign 4", template)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "think.S"
            obj = directory / "think.o"
            extracted = directory / "think.bin"
            source.write_text(template.replace("@TAB5_THINK_TORSO_RAW_BIN@",
                                               str(raw_asset.OUTPUT)))
            subprocess.run([str(compiler), "-c", str(source), "-o", str(obj)], check=True)
            readelf = compiler.with_name("riscv32-esp-elf-readelf")
            sections = subprocess.check_output([str(readelf), "-SW", str(obj)], text=True)
            symbols = subprocess.check_output([str(readelf), "-sW", str(obj)], text=True)
            self.assertRegex(sections, r"\.rodata\.embedded\s+PROGBITS\s+\S+\s+\S+\s+\S+\s+\S+\s+A\s+\d+\s+\d+\s+4")
            import re
            start = re.search(r"^\s*\d+:\s+([0-9a-f]+).*_binary_think_torso_262x462_rgb565a8_bin_start$",
                              symbols, re.M)
            end = re.search(r"^\s*\d+:\s+([0-9a-f]+).*_binary_think_torso_262x462_rgb565a8_bin_end$",
                            symbols, re.M)
            self.assertIsNotNone(start)
            self.assertIsNotNone(end)
            self.assertEqual(int(start[1], 16) % 4, 0)
            self.assertEqual(int(end[1], 16) - int(start[1], 16), len(self.data))
            objcopy = compiler.with_name("riscv32-esp-elf-objcopy")
            subprocess.run([str(objcopy), "-O", "binary", "--only-section=.rodata.embedded",
                            str(obj), str(extracted)], check=True)
            self.assertEqual(extracted.read_bytes(), self.data)

    def test_image_descriptor_compiles_as_raw_rgb565a8(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "lvgl.h").write_text(
                "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
                "#define LV_IMAGE_HEADER_MAGIC 0x19\n"
                "#define LV_COLOR_FORMAT_RGB565A8 0x1\n"
                "typedef struct { unsigned magic, cf, stride, w, h; } lv_image_header_t;\n"
                "typedef struct { lv_image_header_t header; size_t data_size; "
                "const uint8_t *data; } lv_image_dsc_t;\n")
            subprocess.run(
                ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(directory),
                 "-I", str(ROOT / "main/boards/qdtech/tab5"), "-c",
                 str(ROOT / "main/boards/qdtech/tab5/nabo_think_torso_raw.c"),
                 "-o", str(directory / "descriptor.o")],
                check=True)


if __name__ == "__main__":
    unittest.main()
