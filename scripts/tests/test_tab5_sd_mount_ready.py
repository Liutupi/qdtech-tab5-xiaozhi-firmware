"""Run the production SD worker loop with controlled mount/IO events on host."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_tab5_sd_scene import fixture

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


def harness(scene_source, mount_source):
    begin = scene_source.index("    void Loop() {")
    loop = scene_source[begin:scene_source.index("    SdSource source_;", begin)]
    ready = re.search(r"static [^\n]*s_ready[^\n]*;", mount_source).group(0)
    getter = mount_source[mount_source.index("bool Tab5SdReady() {"):]
    return r'''
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "nabo_scene_mailbox.h"
template<typename... Args> void FakeLog(const char*, const char*, Args...) {}
#define ESP_LOGI(...) FakeLog(__VA_ARGS__)
#define ESP_LOGW(...) FakeLog(__VA_ARGS__)
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_INTERNAL = 2;
unsigned heap_caps_get_minimum_free_size(unsigned) { return 0; }
unsigned uxTaskGetStackHighWaterMark(void*) { return 0; }
#define pdMS_TO_TICKS(x) (x)
constexpr size_t kBytes = 320 * 412 * 3;
constexpr const char* kPaths[] = {"idle", "transition", "sleep", "work"};
constexpr unsigned kCounts[] = {113, 81, 207, 50};
constexpr unsigned kMaxConsecutiveTimeouts = 8;
constexpr uint32_t kTimeoutBackoffMs = 250;
constexpr size_t kReadChunk = 64 * 1024;
constexpr uint32_t kCallDeadlineMs = 150, kReadDeadlineMs = 200, kFrameDeadlineMs = 260;
static unsigned slow_reads;
static uint64_t now_ms;
static unsigned ticks, limit, mount_at, restore_file_at, cancel_at;
static unsigned opens, premature_opens, reads, displayed, yielded;
static bool file_present, slow_io;
static std::vector<uint8_t> original, data;
''' + ready + '\n' + getter + r'''
int64_t esp_timer_get_time() { return int64_t(now_ms * 1000); }
void vTaskDelay(unsigned ms);
struct Stop {};
struct SdSource : nabo_sd::Source {
    bool Open(const char*) {
        ++opens;
        if (!Tab5SdReady()) { ++premature_opens; return false; }
        return file_present;
    }
    void Close() {}
    uint64_t Size() const override { return data.size(); }
    uint64_t NowMs() const override { return now_ms; }
    size_t ReadAt(uint32_t at, uint8_t* out, size_t n) override {
        ++reads;
        if (slow_io || reads <= slow_reads) now_ms += 200;  // beyond one-call deadline
        if (uint64_t(at) + n > data.size()) return 0;
        std::memcpy(out, data.data() + at, n);
        return n;
    }
};
class Reader {
public:
    std::vector<uint8_t> a, b;
    nabo_sd::SceneMailbox mailbox;
    SdSource source_;
    nabo_sd::Pack pack_;
    std::atomic<uint32_t> failures_{0};
    Reader() : a(kBytes), b(kBytes), mailbox(a.data(), b.data(), kBytes) {}
    bool Failed(unsigned clip) { return failures_.load() & (1u << clip); }
''' + loop + r'''
};
static Reader* reader;
void vTaskDelay(unsigned ms) {
    assert(ms >= 4); // The real loop must always yield, including absent-card paths.
    ++yielded;
    now_ms += ms;
    ++ticks;
    auto lease = reader->mailbox.TakeReady();
    if (lease.slot >= 0) { ++displayed; reader->mailbox.Release(lease); }
    if (ticks >= limit) throw Stop{};
    if (ticks >= mount_at) s_ready = true;
    if (ticks >= restore_file_at) file_present = true;
    if (ticks >= cancel_at) reader->mailbox.CancelAll();
    else reader->mailbox.Request(0, (ticks / 10) % 113, 1);
}
enum Case { Delayed, Absent, MissingFile, BadCrc, BadHeader, SlowIo, CancelBeforeMount, TransientSlow };
void run(Case which) {
    now_ms = ticks = opens = premature_opens = reads = displayed = yielded = 0;
    s_ready = false; file_present = true; slow_io = false; slow_reads = 0;
    mount_at = 20; restore_file_at = cancel_at = 0xffffffffu; limit = 100;
    data = original;
    if (which == Delayed) { mount_at = 1800; limit = 1880; }
    if (which == Absent) { mount_at = 0xffffffffu; limit = 500; }
    if (which == MissingFile) { file_present = false; restore_file_at = 60; }
    if (which == BadCrc) data[nabo_sd::U32(data.data()+32)+5] ^= 1;
    if (which == BadHeader) data[0] ^= 1;
    if (which == SlowIo) slow_io = true;
    if (which == CancelBeforeMount) { mount_at = 60; cancel_at = 20; }
    if (which == TransientSlow) slow_reads = kMaxConsecutiveTimeouts - 1;
    Reader r; reader = &r; r.mailbox.Request(0, 0, 1);
    try { r.Loop(); } catch (const Stop&) {}
    assert(yielded == limit && premature_opens == 0);
    if (which == Delayed) {
        assert(opens == 1 && displayed > 0 && !r.Failed(0));
        std::puts("PASS delayed mount at 7.2s recovers: zero early IO, one open, frames delivered");
    } else if (which == TransientSlow) {
        assert(opens == slow_reads + 1 && displayed > 0 && !r.Failed(0));
        std::puts("PASS transient read timeouts below the limit skip frames and recover");
    } else if (which == Absent || which == CancelBeforeMount) {
        assert(opens == 0 && reads == 0 && displayed == 0 && !r.Failed(0));
        std::puts("PASS absent card / cancelled pending request: static, no IO, no permanent fault, yields");
    } else if (which == SlowIo) {
        // Header reads time out too: bounded retries with backoff, then sticky.
        assert(opens == kMaxConsecutiveTimeouts && reads == kMaxConsecutiveTimeouts);
        assert(displayed == 0 && r.Failed(0));
        std::puts("PASS persistent timeout: bounded retries with backoff, then sticky fallback");
    } else {
        assert(opens == 1 && displayed == 0 && r.Failed(0));
        std::puts("PASS genuine missing file / CRC / bad header: one open, sticky fallback");
    }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    original.assign(std::istreambuf_iterator<char>(file), {});
    assert(!original.empty());
    for (Case c : {Delayed, Absent, MissingFile, BadCrc, BadHeader, SlowIo, CancelBeforeMount, TransientSlow})
        run(c);
}
'''


class Tab5SdMountReadyTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_real_worker_delayed_mount_faults_and_cancellation(self):
        source = harness((BOARD / "tab5_sd_scene.cc").read_text(),
                         (BOARD / "tab5_sd.cc").read_text())
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            pack = directory / "idle.nab"
            fixture(pack, 320, 412, list(range(0, 113*40, 40)), 113*40)
            cpp = directory / "worker.cc"
            cpp.write_text(source)
            binary = directory / "worker"
            subprocess.run(["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-I", str(BOARD), str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(pack)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
