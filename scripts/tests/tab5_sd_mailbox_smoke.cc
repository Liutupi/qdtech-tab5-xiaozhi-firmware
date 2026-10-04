#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>
#include "nabo_scene_mailbox.h"
#include "nabo_sd_host_source.h"
using namespace nabo_sd;
struct Intercept : Source {
    FileSource source;
    std::function<void()> on_read;
    explicit Intercept(const char* p) : source(p) {}
    uint64_t Size() const override { return source.Size(); }
    uint64_t NowMs() const override { return source.NowMs(); }
    size_t ReadAt(uint32_t o, uint8_t* p, size_t n) override {
        auto bytes = source.ReadAt(o, p, n);
        if (on_read)
            on_read();
        return bytes;
    }
};
int main(int argc, char** argv) {
    assert(argc == 2);
    constexpr size_t bytes = 320 * 412 * 3;
    std::vector<uint8_t> a(bytes), b(bytes);
    Intercept source(argv[1]);
    Pack pack;
    assert(pack.Open(source, bytes, 10000) == Error::Ok);
    SceneMailbox box(a.data(), b.data(), bytes);
    box.Request(0, 0, 1);
    assert(box.Pump(pack, box.Requested()) == Error::Ok);
    auto first = box.TakeReady();
    assert(first.slot >= 0);
    const auto crc = Crc(first.pixels, bytes);
    box.Request(0, 1, 1);
    assert(box.Pump(pack, box.Requested()) == Error::Ok);
    auto second = box.TakeReady();
    assert(second.slot >= 0 && second.slot != first.slot);
    for (unsigned i = 2; i < 12; ++i) {
        box.Request(0, i, 1);
        box.Pump(pack, box.Requested());
    }
    assert(Crc(first.pixels, bytes) == crc);
    assert(box.TakeReady().slot < 0);
    box.Release(first);
    box.Release(second);
    // Same-epoch latest requests must not starve a slow in-flight read.
    box.Request(0, 12, 1);
    auto token = box.Requested();
    unsigned reads = 0;
    source.on_read = [&] {
        if (++reads == 1)
            box.Request(0, 13, 1);
    };
    assert(box.Pump(pack, token) == Error::Ok);
    auto late = box.TakeReady();
    assert((late.token & 255) == 12);
    box.Release(late);
    source.on_read = {};
    box.Pump(pack, box.Requested());
    late = box.TakeReady();
    assert((late.token & 255) == 13);
    box.Release(late);
    // A new state cancels at the next bounded chunk and discards completion.
    box.Request(0, 20, 2);
    token = box.Requested();
    reads = 0;
    source.on_read = [&] {
        if (++reads == 1)
            box.Request(1, 0, 3);
    };
    assert(box.Pump(pack, token) == Error::Cancelled);
    assert(reads == 1);
    assert(box.TakeReady().slot < 0);
    source.on_read = {};
    // Newest completed slot wins, even if slot 1 was written before slot 0.
    box.Request(0, 30, 4);
    box.Pump(pack, box.Requested());
    first = box.TakeReady();
    box.Request(0, 31, 4);
    box.Pump(pack, box.Requested());
    box.Release(first);
    box.Request(0, 32, 4);
    box.Pump(pack, box.Requested());
    late = box.TakeReady();
    assert((late.token & 255) == 32);
    box.Release(late);
    assert(box.TakeReady().slot < 0);
    box.Request(0, 40, 5);
    box.Pump(pack, box.Requested());
    box.CancelAll();
    assert(box.TakeReady().slot < 0);
    box.Request(4, 0, 1);
    assert(box.Requested() == 0);
    box.Request(0, 256, 1);
    assert(box.Requested() == 0);
    assert(source.source.max_request <= 8192);
    // DMA placement: frame starts at base + offset % align, so every sector-aligned
    // file position maps to an align-aligned destination; capacity includes padding.
    {
        constexpr size_t align = 128, slot = bytes + align;
        auto* x = static_cast<uint8_t*>(std::aligned_alloc(align, slot + align));
        auto* y = static_cast<uint8_t*>(std::aligned_alloc(align, slot + align));
        assert(x && y);
        SceneMailbox aligned(x, y, slot, align);
        for (unsigned i = 0; i < 4; ++i) {
            aligned.Request(0, i, 9);
            assert(aligned.Pump(pack, aligned.Requested()) == Error::Ok);
            auto lease = aligned.TakeReady();
            assert(lease.slot >= 0);
            const auto entry = pack.At(i);
            const uint8_t* base = lease.slot ? y : x;
            assert(lease.pixels == base + entry.offset % align);
            const size_t to_sector = (512 - entry.offset % 512) % 512;
            assert(uintptr_t(lease.pixels + to_sector) % align == 0);
            assert(Crc(lease.pixels, entry.bytes) == entry.crc);
            aligned.Release(lease);
        }
        std::free(x);
        std::free(y);
    }
    // Verify-once: first read of a frame index checks CRC, repeats skip it, a
    // fresh Open verifies again; an unverified corrupt frame still fails.
    {
        Pack once;
        assert(once.Open(source, bytes, 10000) == Error::Ok);
        once.SetVerifyOnce(true);
        FrameTrace trace;
        assert(once.Frame(5, a.data(), bytes, {}, &trace) == Error::Ok);
        assert(trace.crc_bytes == bytes && trace.phase == FrameTrace::Complete);
        assert(once.Frame(5, a.data(), bytes, {}, &trace) == Error::Ok);
        assert(trace.crc_bytes == 0 && trace.phase == FrameTrace::Complete);
        assert(Crc(a.data(), bytes) == once.At(5).crc);
        assert(once.Frame(6, a.data(), bytes, {}, &trace) == Error::Ok && trace.crc_bytes == bytes);
        assert(once.Open(source, bytes, 10000) == Error::Ok);
        once.SetVerifyOnce(true);
        assert(once.Frame(5, a.data(), bytes, {}, &trace) == Error::Ok && trace.crc_bytes == bytes);
    }
    // Third slot: the reader publishes while one frame is shown and one waits.
    {
        std::vector<uint8_t> c(bytes);
        SceneMailbox three(a.data(), b.data(), bytes, 1, c.data());
        three.Request(0, 1, 7);
        assert(three.Pump(pack, three.Requested()) == Error::Ok);
        auto shown = three.TakeReady();
        three.Request(0, 2, 7);
        assert(three.Pump(pack, three.Requested()) == Error::Ok);  // slot 2 ready
        three.Request(0, 3, 7);
        SceneMailbox::PumpTrace trace;
        assert(three.Pump(pack, three.Requested(), &trace) == Error::Ok && trace.published);
        auto newest = three.TakeReady();  // newest wins, the older ready slot is freed
        assert(newest.slot >= 0 && (newest.token & 255) == 3 && newest.slot != shown.slot);
        three.Release(shown);
        three.Release(newest);
    }
    // Firmware read size: whole-frame reads use 64KiB calls, CRC stays chunked.
    {
        Pack large;
        assert(large.Open(source, bytes, 10000) == Error::Ok);
        large.SetReadChunk(64 * 1024);
        FrameTrace trace;
        assert(large.Frame(3, a.data(), bytes, {}, &trace) == Error::Ok);
        assert(trace.read_chunks == (bytes + 65535) / 65536 && trace.read_bytes == bytes);
        assert(trace.crc_chunks == (bytes + kChunk - 1) / kChunk);
        assert(source.source.max_request == 64 * 1024);
    }
    std::puts(
        "PASS held-buffer immutability; slow-read frame skipping; epoch cancellation; newest "
        "completion selection; stale discard; invalid request bounds; 8192-byte reads; "
        "DMA-aligned frame placement; 64KiB firmware reads; verify-once CRC; "
        "third slot");
}
