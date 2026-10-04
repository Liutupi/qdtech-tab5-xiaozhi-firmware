"""Check the ESP CRC adapter against zlib and the SDK's reference implementation."""
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

from test_tab5_sd_scene import BOARD, TESTS, fixture


class Tab5SdRomCrcTests(unittest.TestCase):
    def test_rom_adapter_matches_zlib_and_runtime_contracts(self):
        sdk = Path(os.environ.get("IDF_PATH", ""))
        reference = sdk / "components/esp_rom/patches/esp_rom_crc.c"
        includes = sdk / "components/esp_rom/include"
        if not reference.is_file():
            self.skipTest("Set IDF_PATH to run the ESP CRC path against the SDK reference")
        if not shutil.which("cc") or not shutil.which("c++"):
            self.skipTest("C/C++ host compilers required")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            # Compile the SDK source itself, not a copied or mocked CRC algorithm.
            (directory / "esp_rom_caps.h").write_text(
                "#define ESP_ROM_HAS_CRC_BE 1\n#define ESP_ROM_HAS_CRC_LE 0\n")
            obj = directory / "rom.o"
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I", str(directory), "-c", str(reference), "-o", str(obj)], check=True)
            data = random.Random(20261004).randbytes(395520 + 8)
            (directory / "data.bin").write_bytes(data)
            cases = bytearray()
            for offset in range(8):
                for length in [0, 1, 7, 8, 31, 60, 64, 3616, 8191, 8192, 8193, 395520]:
                    for seed in [0, 1, 0xffffffff, 0x12345678]:
                        expected = zlib.crc32(data[offset:offset + length], seed)
                        cases += struct.pack("<IIII", offset, length, seed, expected)
            (directory / "cases.bin").write_bytes(cases)
            harness = directory / "crc.cc"
            harness.write_text(r'''
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "nabo_framepack.h"
using namespace nabo_sd;
std::vector<uint8_t> Load(const char* name) {
    std::ifstream file(name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
    assert(argc == 3);
    const auto data = Load(argv[1]), cases = Load(argv[2]);
    assert(!data.empty() && cases.size() % 16 == 0);
    assert(Crc(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926u);
    assert(CrcStep(0x12345678u, nullptr, 0) == 0x12345678u);
    for (size_t at = 0; at < cases.size(); at += 16) {
        const auto* record = cases.data() + at;
        const uint32_t offset = U32(record), length = U32(record+4);
        const uint32_t seed = U32(record+8), expected = U32(record+12);
        assert(size_t(offset) + length <= data.size());
        assert(~CrcStep(~seed, data.data()+offset, length) == expected);
        for (const size_t chunk : {size_t(1), size_t(7), size_t(8192)}) {
            uint32_t state = ~seed;
            for (size_t i = 0; i < length; i += chunk)
                state = CrcStep(state, data.data()+offset+i, std::min(chunk, size_t(length)-i));
            assert(~state == expected);
        }
    }
    std::printf("PASS %zu zlib CRC vectors: arbitrary seeds, zero length, alignment and chunk continuation\n", cases.size()/16);
}
''')
            options = ["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-DESP_PLATFORM", "-I", str(BOARD), "-I", str(includes)]
            binary = directory / "crc"
            subprocess.run(options + [str(harness), str(obj), "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(directory / "data.bin"), str(directory / "cases.bin")],
                           check=True, timeout=30)
            idle = directory / "idle.nab"
            eye = directory / "eye.nab"
            fixture(idle, 320, 412, list(range(0, 113*40, 40)), 113*40)
            fixture(eye, 204, 93, [0, 3280, 3360, 3440, 3480], 5000)
            # Same actual parser/mailbox harnesses with ESP_PLATFORM enabled.
            for name, pack in [("pack", eye), ("mailbox", idle), ("trace", idle)]:
                binary = directory / name
                subprocess.run(options + [str(TESTS / f"tab5_sd_{name}_smoke.cc"),
                                          str(obj), "-o", str(binary)], check=True)
                subprocess.run([str(binary), str(pack)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
