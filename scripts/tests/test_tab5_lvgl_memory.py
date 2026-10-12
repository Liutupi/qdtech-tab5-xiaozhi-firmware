"""Exercise the actual native LVGL allocator, including low-memory realloc."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/tab5_lvgl_memory.cc"


class Tab5LvglMemoryTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_small_objects_and_realloc_stay_in_psram(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "sdkconfig.h").write_text(
                "#define CONFIG_QDTECH_TAB5_NATIVE_UI 1\n"
                "#define CONFIG_LV_USE_CUSTOM_MALLOC 1\n#define CONFIG_SPIRAM 1\n")
            (path / "esp_log.h").write_text("#define ESP_LOGE(...) ((void)0)\n")
            (path / "esp_heap_caps.h").write_text(r'''
#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
void* heap_caps_malloc(size_t, uint32_t);
void* heap_caps_realloc(void*, size_t, uint32_t);
void heap_caps_free(void*);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
''')
            (path / "lvgl.h").write_text(r'''
#pragma once
#include <cstddef>
using lv_mem_pool_t = void*;
struct lv_mem_monitor_t {};
enum lv_result_t { LV_RESULT_OK };
extern "C" {
void* lv_malloc_core(size_t);
void* lv_realloc_core(void*, size_t);
void lv_free_core(void*);
void lv_mem_init();
void lv_mem_deinit();
lv_mem_pool_t lv_mem_add_pool(void*, size_t);
void lv_mem_remove_pool(lv_mem_pool_t);
void lv_mem_monitor_core(lv_mem_monitor_t*);
lv_result_t lv_mem_test_core();
}
''')
            (path / "smoke.cc").write_text(r'''
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
std::unordered_map<void*, size_t> blocks;
bool fail = false;
void* heap_caps_malloc(size_t n, uint32_t caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail) return nullptr;
    void* p = malloc(n); if (p) blocks[p] = n; return p;
}
void heap_caps_free(void* p) {
    if (p) { assert(blocks.erase(p) == 1); free(p); }
}
void* heap_caps_realloc(void* p, size_t n, uint32_t caps) {
    if (!n) { heap_caps_free(p); return nullptr; }
    void* q = heap_caps_malloc(n, caps);
    if (q && p) { memcpy(q, p, std::min(n, blocks.at(p))); heap_caps_free(p); }
    return q;
}
size_t heap_caps_get_free_size(uint32_t) { return 0; }
size_t heap_caps_get_largest_free_block(uint32_t) { return 0; }
int main() {
    lv_mem_init();
    std::vector<void*> page;
    for (int round = 0; round < 20; ++round) {
        for (size_t i = 1; i <= 1024; ++i) page.push_back(lv_malloc_core(i));
        for (auto p : page) { assert(p); lv_free_core(p); }
        page.clear(); assert(blocks.empty());
    }
    auto p = static_cast<unsigned char*>(lv_realloc_core(nullptr, 4096));
    memset(p, 0xa5, 4096);
    p = static_cast<unsigned char*>(lv_realloc_core(p, 32));
    for (int i = 0; i < 32; ++i) assert(p[i] == 0xa5);
    fail = true;
    assert(!lv_malloc_core(48));
    assert(!lv_realloc_core(p, 8192));
    assert(blocks.at(p) == 32 && p[0] == 0xa5);  // failure preserves old storage
    fail = false;
    assert(!lv_realloc_core(p, 0));
    lv_free_core(nullptr);
    assert(blocks.empty());
    assert(lv_mem_add_pool(nullptr, 0) == nullptr);
    lv_mem_remove_pool(nullptr);
    assert(lv_mem_test_core() == LV_RESULT_OK);
    lv_mem_deinit();
}
''')
            binary = path / "smoke"
            subprocess.run(["c++", "-std=c++17", "-fsanitize=address,undefined",
                            "-I", str(path), str(SOURCE), str(path / "smoke.cc"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
