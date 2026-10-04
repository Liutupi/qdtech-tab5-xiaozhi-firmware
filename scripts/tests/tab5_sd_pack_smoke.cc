#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <new>
#include <vector>
#include "nabo_framepack.h"
#include "nabo_sd_host_source.h"
using namespace nabo_sd;
static size_t allocations = 0;
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

struct Memory : Source {
    std::vector<uint8_t> bytes;
    mutable uint64_t clock = 0;
    uint64_t calls = 0, total = 0;
    size_t maximum = 0;
    uint32_t delay = 0;
    bool short_read = false;
    void (*hook)(void*) = nullptr;
    void* hook_arg = nullptr;
    explicit Memory(const std::vector<uint8_t>& input) : bytes(input) {}
    uint64_t Size() const override { return bytes.size(); }
    uint64_t NowMs() const override { return clock; }
    size_t ReadAt(uint32_t offset, uint8_t* out, size_t n) override {
        ++calls;
        total += n;
        maximum = std::max(maximum, n);
        clock += delay;
        if (offset > bytes.size())
            return 0;
        n = std::min(n, bytes.size() - offset);
        if (short_read && n)
            --n;
        std::memcpy(out, bytes.data() + offset, n);
        if (hook) {
            auto h = hook;
            hook = nullptr;
            h(hook_arg);
        }
        return n;
    }
};
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[at + i] = uint8_t(v >> (8 * i));
}
void FixHeader(std::vector<uint8_t>& b) { Put32(b, 60, Crc(b.data(), 60)); }
void FixIndex(std::vector<uint8_t>& b) {
    Put32(b, 48, Crc(b.data() + 64, U32(b.data() + 20) * 32));
    FixHeader(b);
}
int passed = 0;
void Pass(const char* s) {
    ++passed;
    std::printf("PASS %s\n", s);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> base{std::istreambuf_iterator<char>(input), {}};
    assert(!base.empty());
    constexpr size_t capacity = 204 * 93 * 3;
    std::array<uint8_t, capacity> a{}, b{};
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        assert(p.Count() == 5 && p.Duration() == 5000);
        assert(p.Due(3279) == 0 && p.Due(3280) == 1 && p.Due(3360) == 2);
        assert(p.Due(9999) == 4 && p.Due(10000) == 0);
        assert(p.Due((1ULL << 32) + 3360) < p.Count());
        for (uint32_t i = 0; i < p.Count(); ++i)
            assert(p.Frame(i, a.data(), a.size()) == Error::Ok);
        assert(source.maximum <= kChunk);
        assert(p.Frame(500, a.data(), a.size()) == Error::Bounds);
        assert(p.Frame(0, a.data(), 1) == Error::Capacity);
        Pass("valid pack, boundary timeline, loop, long uptime, 8KiB read limit");
    }
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity - 1) == Error::Capacity);
        FileSource missing("/nonexistent/nabo-file-must-not-exist.nab");
        assert(p.Open(missing, capacity) == Error::Io);
        Pass("buffer budget and missing SD/file reject safely");
    }
    {
        auto broken = base;
        broken[0] ^= 1;
        Memory source(broken);
        Pack p;
        assert(p.Open(source, capacity) == Error::Format);
        broken = base;
        broken[64] ^= 1;
        Memory index(broken);
        assert(p.Open(index, capacity) == Error::Crc);
        broken = base;
        Put32(broken, 20, 257);
        FixHeader(broken);
        Memory count(broken);
        assert(p.Open(count, capacity) == Error::Bounds);
        Pass("header, index checksum, count cap");
    }
    {
        auto broken = base;
        Put32(broken, 64 + 4, 0xfffffff0u);
        FixIndex(broken);
        Memory source(broken);
        Pack p;
        assert(p.Open(source, capacity) == Error::Bounds);
        broken = base;
        Put32(broken, 64 + 32, 0);
        FixIndex(broken);
        Memory order(broken);
        assert(p.Open(order, capacity) == Error::Bounds);
        broken = base;
        broken[64 + 20] = 0;
        broken[64 + 21] = 0;
        FixIndex(broken);
        Memory geometry(broken);
        assert(p.Open(geometry, capacity) == Error::Bounds);
        broken = base;
        broken.pop_back();
        Memory truncated(broken);
        assert(p.Open(truncated, capacity) == Error::Bounds);
        Pass("overflow offset, timestamp order, zero dimensions, truncated file");
    }
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        source.bytes[p.At(1).offset] ^= 1;
        assert(p.Frame(1, a.data(), a.size()) == Error::Crc);
        source.short_read = true;
        assert(p.Frame(0, a.data(), a.size()) == Error::Io);
        source.short_read = false;
        source.delay = 30;
        assert(p.Frame(0, a.data(), a.size()) == Error::Timeout);
        Pass("bad payload CRC, runtime card removal/short read, slow read timeout");
    }
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        Player player(p, a.data(), b.data(), capacity);
        Gates g;
        assert(player.Update(0, g));
        assert(player.Pump() == Error::Ok);
        auto old = player.TakeReady();
        assert(old.slot >= 0);
        const uint32_t crc = Crc(old.pixels, old.entry.bytes);
        assert(player.Update(3360, g));
        assert(player.Pump() == Error::Ok);
        auto next = player.TakeReady();
        assert(next.slot >= 0 && next.slot != old.slot);
        assert(player.Update(3440, g));
        const auto reads = source.calls;
        player.Pump();
        assert(source.calls == reads);
        assert(Crc(old.pixels, old.entry.bytes) == crc);
        player.Release(old);
        assert(player.Pump() == Error::Ok);
        auto third = player.TakeReady();
        assert(third.slot == old.slot);
        player.Release(next);
        player.Release(third);
        Pass("two leased buffers, renderer acknowledgement, no held-buffer overwrite");
    }
    {
        for (int gate = 0; gate < 8; ++gate) {
            Memory source(base);
            Pack p;
            assert(p.Open(source, capacity) == Error::Ok);
            Player player(p, a.data(), b.data(), capacity);
            Gates g;
            assert(player.Update(0, g));
            assert(player.Pump() == Error::Ok);
            auto frame = player.TakeReady();
            assert(frame.slot >= 0);
            switch (gate) {
                case 0:
                    g.idle = false;
                    break;
                case 1:
                    g.audio_pending = true;
                    break;
                case 2:
                    g.voice_detected = true;
                    break;
                case 3:
                    g.sleeping = true;
                    break;
                case 4:
                    g.preview = true;
                    break;
                case 5:
                    g.app_visible = true;
                    break;
                case 6:
                    g.gesture = true;
                    break;
                case 7:
                    g.music_playing = true;
                    break;
            }
            const auto reads = source.calls;
            assert(!player.Update(40, g));
            player.Pump();
            assert(source.calls == reads && player.RequestForTest() == 0);
            player.Release(frame);
            assert(player.Update(80, {}));
            assert(player.Pump() == Error::Ok);
            auto resumed = player.TakeReady();
            assert(resumed.slot >= 0 && resumed.entry.at_ms == 0);
            player.Release(resumed);
        }
        Pass("speech, audio drain, VAD, sleep, camera, app, gesture, music priority gates");
    }
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        Player player(p, a.data(), b.data(), capacity);
        assert(player.Update(0, {}));
        source.hook_arg = &player;
        source.hook = [](void* context) {
            Gates g;
            g.idle = false;
            assert(!static_cast<Player*>(context)->Update(40, g));
        };
        const auto reads = source.calls;
        assert(player.Pump() == Error::Cancelled && source.calls == reads + 1);
        assert(player.TakeReady().slot < 0);
        assert(player.Update(80, {}));
        assert(player.Pump() == Error::Ok);
        auto ready = player.TakeReady();
        assert(ready.slot >= 0);
        player.Release(ready);
        Pass(
            "interrupt during a read discards completion before publish; resume uses fresh "
            "generation");
    }
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        Player player(p, a.data(), b.data(), capacity);
        assert(player.Update(0, {}));
        assert(player.Pump() == Error::Ok);  // Deliberately leave ready in mailbox.
        Gates blocked;
        blocked.idle = false;
        assert(!player.Update(1, blocked));
        assert(player.TakeReady().slot < 0);
        assert(player.Update(2, {}));
        assert(player.Pump() == Error::Ok);
        auto fresh = player.TakeReady();
        assert(fresh.slot >= 0);
        player.Release(fresh);
        assert(player.Update(3482, {}));
        assert(player.Pump() == Error::Ok);
        auto skipped = player.TakeReady();
        assert(skipped.slot >= 0 && skipped.entry.at_ms == 3480);
        player.Release(skipped);
        Pass("stale ready frame discarded and delayed UI jumps directly to current frame");
    }
    {
        auto fast = base;
        for (uint32_t i = 0; i < 5; ++i)
            Put32(fast, 64 + i * 32, i * 40);
        FixIndex(fast);
        Memory source(fast);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        Player player(p, a.data(), b.data(), capacity);
        for (uint64_t t = 0; t <= 160; t += 40)
            assert(player.Update(t, {}));
        assert(!player.Update(200, {}));
        assert(player.Fault() == Error::Timeout);
        Pass("unresponsive worker fallback even while requested frame changes every 40ms");
    }
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        Player player(p, a.data(), b.data(), capacity);
        source.bytes[p.At(0).offset] ^= 1;
        assert(player.Update(0, {}));
        assert(player.Pump() == Error::Crc);
        assert(!player.Update(40, {}));
        assert(player.TakeReady().slot < 0);
        assert(!player.Update(1000, {}));
        Pass("corrupt frame faults sticky to static, no auto-retry storm");
    }
    {
        Memory source(base);
        Pack p;
        assert(p.Open(source, capacity) == Error::Ok);
        Player player(p, a.data(), b.data(), capacity);
        Player::Lease held;
        const auto before = allocations;
        for (uint64_t t = 0; t < 60000; t += 40) {
            assert(player.Update(t, {}));
            assert(player.Pump() == Error::Ok);
            auto n = player.TakeReady();
            if (n.slot >= 0) {
                player.Release(held);
                held = n;
            }
        }
        player.Release(held);
        assert(allocations == before);
        Pass("60s simulation: zero C++ heap allocations in update/read/publish loop");
    }
    {
        auto scene = base;
        scene[8] = 2;
        Put32(scene, 40, 2);
        FixHeader(scene);
        Memory source(scene);
        Pack pack;
        assert(pack.Open(source, capacity) == Error::Format);
        assert(pack.Open(source, capacity, 80, true) == Error::Ok);
        scene[8] = 1;
        FixHeader(scene);
        Memory mixed(scene);
        assert(pack.Open(mixed, capacity, 80, true) == Error::Format);
        Pass("scene v2 requires explicit opt-in; original idle v1 policy retained");
    }
    std::printf(
        "RESULT tests=%d payload_buffers=%zu parser_bytes=%zu player_bytes=%zu max_io_chunk=%zu\n",
        passed, 2 * capacity, sizeof(Pack), sizeof(Player), kChunk);
}
