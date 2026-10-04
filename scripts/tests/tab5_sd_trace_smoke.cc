#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <new>
#include <vector>
#include "nabo_scene_mailbox.h"
using namespace nabo_sd;
static size_t allocations;
void* operator new(size_t n) {
    ++allocations;
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
static uint64_t now_us;
static uint32_t crc_step_us;
static FrameTrace* active_trace;
static SceneMailbox* cancel_during_crc;
uint64_t Clock() {
    if (active_trace && active_trace->phase == FrameTrace::CrcPhase) {
        now_us += crc_step_us;
        if (cancel_during_crc && active_trace->crc_chunks >= 2) {
            cancel_during_crc->CancelAll();
            cancel_during_crc = nullptr;
        }
    }
    return now_us;
}
struct Memory : Source {
    std::vector<uint8_t> bytes;
    uint32_t delay_us = 0;
    bool short_read = false;
    SceneMailbox* cancel_on_read = nullptr;
    explicit Memory(const std::vector<uint8_t>& b) : bytes(b) {}
    uint64_t Size() const override { return bytes.size(); }
    uint64_t NowMs() const override { return now_us / 1000; }
    size_t ReadAt(uint32_t off, uint8_t* out, size_t n) override {
        now_us += delay_us;
        if (uint64_t(off) + n > bytes.size())
            return 0;
        if (short_read)
            --n;
        std::memcpy(out, bytes.data() + off, n);
        if (cancel_on_read) {
            cancel_on_read->CancelAll();
            cancel_on_read = nullptr;
        }
        return n;
    }
};
constexpr size_t kBytes = 320 * 412 * 3;
struct Test {
    Memory source;
    Pack pack;
    std::vector<uint8_t> a, b;
    SceneMailbox box;
    SceneMailbox::PumpTrace trace;
    explicit Test(const std::vector<uint8_t>& bytes, uint32_t read_ms = 0, uint32_t total_ms = 0)
        : source(bytes), a(kBytes), b(kBytes), box(a.data(), b.data(), kBytes) {
        now_us = 0;
        crc_step_us = 0;
        cancel_during_crc = nullptr;
        assert(pack.Open(source, kBytes, 80, false, read_ms, total_ms) == Error::Ok);
        trace.frame.clock_us = Clock;
        active_trace = &trace.frame;
        source.delay_us = 100;
    }
    Error Run(unsigned frame = 0) {
        box.Request(0, frame, 1);
        const auto before = allocations;
        const auto result = box.Pump(pack, box.Requested(), &trace);
        assert(allocations == before);  // Instrumented worker has no heap allocations.
        return result;
    }
};
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    assert(!bytes.empty());
    {
        Test t(bytes);
        assert(t.Run() == Error::Ok && t.trace.published && t.trace.attempted);
        assert(t.trace.frame.phase == FrameTrace::Complete);
        assert(t.trace.frame.read_bytes == kBytes && t.trace.frame.crc_bytes == kBytes);
        assert(t.trace.frame.read_chunks == 49 && t.trace.frame.crc_chunks == 49);
        assert(t.trace.frame.read_us == 4900 && t.trace.frame.read_max_us == 100);
        const auto first = t.box.TakeReady();
        assert(first.slot >= 0);
        assert(t.Run(1) == Error::Ok && t.trace.published);
        const auto second = t.box.TakeReady();
        assert(second.slot >= 0);
        assert(t.Run(2) == Error::Ok && t.trace.busy && !t.trace.published && !t.trace.attempted);
        assert(t.trace.frame.read_bytes == 0 && t.trace.frame.phase == FrameTrace::None);
        t.box.Release(first);
        t.box.Release(second);
        assert(t.Run(2) == Error::Ok && t.trace.published);
        assert(t.Run(2) == Error::Cancelled && !t.trace.attempted && !t.trace.published);
        std::puts(
            "PASS exact full-frame byte/chunk/time metrics; actual publish vs busy; no allocation");
    }
    {
        Test t(bytes);
        t.source.delay_us = 90001;
        assert(t.Run() == Error::Timeout && !t.trace.published);
        assert(t.trace.frame.phase == FrameTrace::ReadPhase);
        assert(t.trace.frame.read_chunks == 1 && t.trace.frame.read_bytes == 8192);
        assert(t.trace.frame.read_us == 90001 && t.trace.frame.read_max_us == 90001);
        assert(t.trace.frame.crc_bytes == 0);
        std::puts(
            "PASS one blocking SD chunk timeout is attributed to read and records returned bytes");
    }
    {
        Test t(bytes);
        t.source.delay_us = 2000;
        assert(t.Run() == Error::Timeout && t.trace.frame.phase == FrameTrace::ReadPhase);
        assert(t.trace.frame.read_chunks == 41 && t.trace.frame.read_us == 82000);
        assert(t.trace.frame.read_max_us == 2000 && t.trace.frame.crc_bytes == 0);
        std::puts("PASS cumulative read timeout distinguished from an individually slow chunk");
    }
    {
        Test t(bytes);
        crc_step_us = 2000;
        assert(t.Run() == Error::Timeout && t.trace.frame.phase == FrameTrace::CrcPhase);
        assert(t.trace.frame.read_bytes == kBytes && t.trace.frame.read_us == 4900);
        assert(t.trace.frame.crc_bytes > 0 && t.trace.frame.crc_bytes < kBytes);
        assert(t.trace.frame.crc_us > 80000 && t.trace.frame.crc_max_us == 2000);
        assert(!t.trace.published && t.box.TakeReady().slot < 0);
        std::puts("PASS CRC deadline attributed separately, original 80ms bound preserved");
    }
    {
        Test t(bytes);
        t.source.bytes[t.pack.At(0).offset] ^= 1;
        assert(t.Run() == Error::Crc && t.trace.frame.phase == FrameTrace::CrcPhase);
        assert(t.trace.frame.read_bytes == kBytes && t.trace.frame.crc_bytes == kBytes);
        assert(!t.trace.published && t.box.TakeReady().slot < 0);
        std::puts("PASS corrupted frame still fails CRC without publication");
    }
    {
        Test t(bytes);
        t.source.cancel_on_read = &t.box;
        assert(t.Run() == Error::Cancelled && t.trace.frame.phase == FrameTrace::ReadPhase);
        assert(t.trace.frame.read_chunks == 1 && t.trace.frame.crc_bytes == 0);
        assert(!t.trace.published && t.box.TakeReady().slot < 0);
        std::puts("PASS read cancellation retains phase and discards partial buffer");
    }
    {
        Test t(bytes);
        cancel_during_crc = &t.box;
        assert(t.Run() == Error::Cancelled && t.trace.frame.phase == FrameTrace::CrcPhase);
        assert(t.trace.frame.crc_bytes < kBytes && !t.trace.published);
        assert(t.box.TakeReady().slot < 0);
        std::puts("PASS CRC cancellation retains phase and never publishes stale frame");
    }
    {
        Test t(bytes);
        t.source.short_read = true;
        assert(t.Run() == Error::Io && t.trace.frame.phase == FrameTrace::ReadPhase);
        assert(t.trace.frame.read_bytes == 8191 && t.trace.frame.read_chunks == 1);
        assert(t.trace.frame.crc_bytes == 0 && !t.trace.published);
        std::puts("PASS short read remains genuine IO fault");
    }
    {
        Test legacy(bytes);
        legacy.source.delay_us = 1800;
        assert(legacy.Run() == Error::Timeout);
        Test t(bytes, 120, 160);
        t.source.delay_us = 1800;
        assert(t.Run() == Error::Ok && t.trace.published);
        assert(t.trace.frame.read_bytes == kBytes && t.trace.frame.read_us == 88200);
        std::puts("PASS measured-throughput frame completes within bounded 120ms read budget");
    }
    {
        Test t(bytes, 120, 160);
        t.source.delay_us = 90001;
        assert(t.Run() == Error::Timeout && t.trace.frame.read_chunks == 1);
        assert(t.trace.frame.phase == FrameTrace::ReadPhase && !t.trace.published);
        std::puts("PASS original 80ms single-call protection retained despite frame read budget");
    }
    {
        Test t(bytes, 120, 160);
        t.source.delay_us = 3000;
        assert(t.Run() == Error::Timeout && t.trace.frame.read_us == 123000);
        assert(!t.trace.published && t.trace.frame.crc_bytes == 0);
        std::puts("PASS 120ms cumulative deadline still rejects an excessively slow frame");
    }
    {
        Test t(bytes, 120, 160);
        t.source.delay_us = 2100;
        crc_step_us = 800;
        assert(t.Run() == Error::Timeout && t.trace.frame.phase == FrameTrace::CrcPhase);
        assert(t.trace.frame.read_us == 102900 && t.trace.frame.crc_us < 80000);
        assert(now_us > 160000 && !t.trace.published);
        std::puts(
            "PASS whole frame 160ms bound combines read and CRC even if each is within budget");
    }

    {
        Test t(bytes, 120, 160);
        crc_step_us = 2000;
        assert(t.Run() == Error::Timeout && t.trace.frame.phase == FrameTrace::CrcPhase);
        assert(t.trace.frame.read_us == 4900 && t.trace.frame.crc_us > 80000);
        assert(!t.trace.published);
        std::puts("PASS scene frame CRC still has its independent 80ms deadline");
    }
}
