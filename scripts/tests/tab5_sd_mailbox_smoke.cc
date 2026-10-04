#include <cassert>
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
    std::puts(
        "PASS held-buffer immutability; slow-read frame skipping; epoch cancellation; newest "
        "completion selection; stale discard; invalid request bounds; 8192-byte reads");
}
