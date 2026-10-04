"""Exercise the production FILE adapter's absolute-offset and sequential cursor contract."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_tab5_sd_scene import fixture
ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/qdtech/tab5'

class Tab5SdSourceCursorTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ host compiler required')
    def test_production_adapter_sequential_random_short_reads_and_seek_failure(self):
        source = (BOARD / 'tab5_sd_scene.cc').read_text()
        start = source.index('class SdSource :')
        adapter = source[start:source.index('lv_obj_t* Box(', start)]
        # Count actual stdio operations without duplicating the adapter algorithm.
        adapter = adapter.replace('std::fseek(', 'CountSeek(').replace('std::fread(', 'CountRead(')
        harness = r'''
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "nabo_framepack.h"
static unsigned seeks;
static bool fail_seek, short_read;
int CountSeek(FILE* f, long offset, int whence) {
    ++seeks;
    if (fail_seek) { fail_seek = false; return -1; }
    return std::fseek(f, offset, whence);
}
size_t CountRead(void* out, size_t size, size_t n, FILE* f) {
    if (short_read) { short_read = false; --n; }
    return std::fread(out, size, n, f);
}
int64_t esp_timer_get_time() { return 0; }
''' + adapter + r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> expected{std::istreambuf_iterator<char>(input), {}};
    std::vector<uint8_t> out(8192);
    SdSource source;
    assert(source.Open(argv[1]) && source.Size() == expected.size() && seeks == 1);
    auto read = [&](uint32_t at, size_t n) {
        assert(source.ReadAt(at, out.data(), n) == n);
        assert(std::memcmp(out.data(), expected.data() + at, n) == 0);
    };
    read(0, 512); assert(seeks == 2);
    read(512, 512); assert(seeks == 2);
    read(4096, 512); assert(seeks == 3);
    fail_seek = true;
    assert(source.ReadAt(16384, out.data(), 512) == 0 && seeks == 4);
    read(4608, 512); assert(seeks == 5); // Failed seek invalidates cached cursor.
    short_read = true;
    assert(source.ReadAt(5120, out.data(), 512) == 511 && seeks == 5);
    assert(std::memcmp(out.data(), expected.data()+5120, 511) == 0);
    read(5631, 512); assert(seeks == 5); // Advance by bytes actually returned.
    read(5631, 512); assert(seeks == 6); // Repeating offset is a real timeline jump.
    const unsigned before = seeks;
    constexpr size_t bytes = 320 * 412 * 3;
    for (size_t at = 0; at < bytes; at += 8192) read(at, std::min(size_t(8192), bytes-at));
    assert(seeks == before + 1); // One absolute seek, then 48 consecutive chunks.
    source.Close();
    assert(source.ReadAt(0, out.data(), 512) == 0);
    assert(source.Open(argv[1]));
    const unsigned reopened = seeks;
    read(0, 512); assert(seeks == reopened + 1);
    std::puts("PASS real FILE adapter: sequential reads skip 48 seeks; random/repeated offsets, short reads, failed seek and reopen remain correct");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            pack = directory / 'idle.nab'
            fixture(pack, 320, 412, list(range(0,113*40,40)),113*40)
            cpp = directory / 'source.cc'; cpp.write_text(harness)
            binary = directory / 'source'
            subprocess.run(['c++','-std=c++17','-O1','-Wall','-Wextra','-Werror',
                            '-I',str(BOARD),str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary),str(pack)],check=True,timeout=15)

if __name__ == '__main__':
    unittest.main()
