"""Checks the optional Tab5 portrait asset without enabling the experiment."""

import importlib.util
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/prepare_nabo_portrait_matte.py"
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("prepare_nabo_portrait_matte", SCRIPT)
matte = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matte)


class Tab5PortraitMatteTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.asset = matte.OUTPUT.read_bytes()
        cls.source = matte.portrait_source().tobytes()
        cls.coverage = matte.inner_coverage()

    def pixel(self, x, y):
        offset = (y * matte.WIDTH + x) * 2
        return int.from_bytes(self.asset[offset:offset + 2], "little")

    def test_asset_is_deterministic_rgb565(self):
        self.assertEqual(len(self.asset), 434 * 558 * 2)
        self.assertEqual(self.asset, matte.compose())

    def test_card_and_portrait_boundaries(self):
        self.assertEqual(len(self.coverage), 434 * 558)
        self.assertEqual(self.coverage[0], 0)
        self.assertEqual(self.coverage[(558 - 1) * 434 + 433], 0)
        self.assertEqual(self.coverage[300 * 434 + 200], 255)
        self.assertTrue(any(0 < value < 255 for value in self.coverage))
        self.assertEqual(matte.PORTRAIT_X + matte.WIDTH, 473)
        self.assertLessEqual(matte.HEIGHT, 558)  # No pixels beyond emoji_box.

    def test_transparent_and_opaque_pixels(self):
        zeros = solids = 0
        for index in range(matte.WIDTH * matte.HEIGHT):
            offset = index * 4
            alpha = self.source[offset + 3]
            pixel = int.from_bytes(self.asset[index * 2:index * 2 + 2], "little")
            if alpha == 0:
                border = index // matte.WIDTH in (0, matte.HEIGHT - 1)
                base = matte.rgb565(matte.OUTER_BORDER if border else matte.OUTER_FILL)
                expected = matte.mix565(matte.rgb565(matte.INNER_FILL), base,
                                        self.coverage[index])
                self.assertEqual(pixel, expected)
                zeros += 1
            elif alpha == 255:
                self.assertEqual(pixel, matte.rgb565(self.source[offset:offset + 3]))
                solids += 1
        self.assertGreater(zeros, 50000)
        self.assertGreater(solids, 100)

    def test_resource_is_gated_and_original_is_off_path(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        display = (ROOT / "main/boards/qdtech/tab5/tab5_native_display.h").read_text()
        self.assertIn("config QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT\n", kconfig)
        self.assertRegex(kconfig, r"config QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT\n(?:.*\n)*?    default n")
        self.assertIn("list(FILTER BOARD_SOURCES EXCLUDE REGEX \"/nabo_portrait_matte", cmake)
        self.assertIn("if(CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT)", cmake)
        self.assertIn("nabo_portrait_matte_embedded.S.in", cmake)
        self.assertIn("${TAB5_PORTRAIT_MATTE_ASM})", cmake)
        self.assertIn("OBJECT_DEPENDS ${TAB5_PORTRAIT_MATTE_BIN}", cmake)
        setup = display.split("portrait_ = lv_image_create(emoji_box_);", 1)[1].split(
            "lv_obj_add_flag(portrait_, LV_OBJ_FLAG_CLICKABLE);", 1)[0]
        enabled, disabled = setup.split("#else", 1)
        self.assertIn("portrait_matte_valid_ = nabo_portrait_matte_available();", enabled)
        self.assertIn("lv_image_set_src(portrait_, &nabo_portrait_matte);", enabled)
        self.assertIn("lv_image_set_src(portrait_, &nabo_sleep);", enabled)
        self.assertIn("lv_image_set_src(sleep_, &nabo_sleep);", display)
        self.assertNotRegex(enabled, r"&nabo_portrait\b")
        self.assertIn("lv_image_set_src(portrait_, &nabo_portrait);", disabled)
        self.assertIn("lv_obj_set_pos(portrait_, 39, 0);", disabled)
        self.assertEqual(len(re.findall(r"&nabo_portrait\b", display)), 1)
        self.assertIn("(wave_active_ || !portrait_matte_valid_) ? 0 : frame.eyes", display)
        self.assertIn("(wave_active_ || !portrait_matte_valid_) ? 0 : frame.mouth", display)
        for define, expected in (([], "&nabo_portrait"),
                                 (["-DCONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT=1"],
                                  "&nabo_portrait_matte")):
            result = subprocess.run(["cc", "-E", "-P", "-x", "c++", *define, "-"],
                                    input=setup, capture_output=True, text=True, check=True)
            self.assertIn(expected, result.stdout)
            if define:
                self.assertNotRegex(result.stdout, r"&nabo_portrait\b")
                self.assertIn("&nabo_sleep", result.stdout)
            else:
                self.assertNotIn("&nabo_portrait_matte", result.stdout)

    def test_configure_checks_size_and_hash(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        validation = cmake.split('file(SIZE "${TAB5_PORTRAIT_MATTE_BIN}"', 1)[1].split(
            "set(TAB5_PORTRAIT_MATTE_ASM", 1)[0]
        validation = 'file(SIZE "${TAB5_PORTRAIT_MATTE_BIN}"' + validation
        enabled_cmake = cmake.split("if(CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT)", 1)[1].split(
            "else()\n", 1)[0]
        self.assertIn(validation, enabled_cmake)
        self.assertIn("file(SHA256", validation)
        self.assertIn("484344", validation)
        self.assertIn(hashlib.sha256(self.asset).hexdigest(), validation)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            script = directory / "validate.cmake"
            for name, data, accepted in (("valid", self.asset, True),
                                         ("same_length_corrupt", bytes([self.asset[0] ^ 1]) + self.asset[1:], False),
                                         ("short", self.asset[:-1], False)):
                candidate = directory / f"{name}.bin"
                candidate.write_bytes(data)
                script.write_text(f'set(TAB5_PORTRAIT_MATTE_BIN "{candidate}")\n{validation}\n')
                result = subprocess.run(["cmake", "-P", str(script)], capture_output=True,
                                        text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)

    def test_incremental_configure_tracks_binary(self):
        if not shutil.which("ninja"):
            self.skipTest("Ninja is needed to inspect CMake regeneration dependencies")
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        enabled = cmake.split("if(CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT)", 1)[1].split(
            "else()\n", 1)[0]
        tracking = enabled.split("set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS", 1)[1].split(
            "file(SIZE", 1)[0]
        tracking = "set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS" + tracking
        validation = enabled.split('file(SIZE "${TAB5_PORTRAIT_MATTE_BIN}"', 1)[1].split(
            "set(TAB5_PORTRAIT_MATTE_ASM", 1)[0]
        validation = 'file(SIZE "${TAB5_PORTRAIT_MATTE_BIN}"' + validation
        self.assertIn(tracking, enabled)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "source"
            build = directory / "build"
            source.mkdir()
            candidate = source / "portrait.bin"
            candidate.write_bytes(self.asset)
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\nproject(matte_dependency NONE)\n"
                f'set(TAB5_PORTRAIT_MATTE_BIN "{candidate}")\n{tracking}\n{validation}\n')
            command = ["cmake", "-G", "Ninja", "-S", str(source), "-B", str(build)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            ninja_file = (build / "build.ninja").read_text()
            self.assertIn(str(candidate), ninja_file)
            self.assertIn("RERUN_CMAKE", ninja_file)
            candidate.write_bytes(bytes([self.asset[0] ^ 1]) + self.asset[1:])
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("failed size/SHA256 validation", result.stderr)

    def test_assembled_flash_asset_has_four_byte_alignment(self):
        compiler = sorted((Path.home() / ".espressif/tools/riscv32-esp-elf").glob(
            "*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"))
        if not compiler:
            self.skipTest("ESP RISC-V assembler is not installed")
        compiler = compiler[-1]
        template = (ROOT / "main/boards/qdtech/tab5/nabo_portrait_matte_embedded.S.in").read_text()
        self.assertIn(".balign 4", template)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "portrait.S"
            obj = directory / "portrait.o"
            extracted = directory / "portrait.bin"
            source.write_text(template.replace("@TAB5_PORTRAIT_MATTE_BIN@", str(matte.OUTPUT)))
            subprocess.run([str(compiler), "-c", str(source), "-o", str(obj)], check=True)
            readelf = compiler.with_name("riscv32-esp-elf-readelf")
            sections = subprocess.check_output([str(readelf), "-SW", str(obj)], text=True)
            symbols = subprocess.check_output([str(readelf), "-sW", str(obj)], text=True)
            self.assertRegex(sections, r"\.rodata\.embedded\s+PROGBITS\s+\S+\s+\S+\s+\S+\s+\S+\s+A\s+\d+\s+\d+\s+4")
            start = re.search(r"^\s*\d+:\s+([0-9a-f]+).*_binary_portrait_matte_434x558_rgb565_bin_start$",
                              symbols, re.M)
            end = re.search(r"^\s*\d+:\s+([0-9a-f]+).*_binary_portrait_matte_434x558_rgb565_bin_end$",
                            symbols, re.M)
            self.assertIsNotNone(start)
            self.assertIsNotNone(end)
            self.assertEqual(int(start[1], 16) % 4, 0)
            self.assertEqual(int(end[1], 16) - int(start[1], 16), len(self.asset))
            objcopy = compiler.with_name("riscv32-esp-elf-objcopy")
            subprocess.run([str(objcopy), "-O", "binary", "--only-section=.rodata.embedded",
                            str(obj), str(extracted)], check=True)
            self.assertEqual(extracted.read_bytes(), self.asset)


if __name__ == "__main__":
    unittest.main()
