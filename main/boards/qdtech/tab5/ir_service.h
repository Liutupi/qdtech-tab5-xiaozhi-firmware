#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// Lightweight IR RX probe + TX via RMT. Used to detect wiring (e.g. "AEC" pad)
// and later send NEC/raw frames from OpenClaw.
class IrService {
public:
    static IrService& GetInstance() {
        static IrService instance;
        return instance;
    }

    // Count edges on each GPIO for `seconds`. Returns JSON: {"gpio":count,...}
    std::string ScanGpios(const int* gpios, size_t n, int seconds);

    // Learn one IR frame on rx_gpio for up to timeout_ms. Returns
    // {"ok":true,"timings_us":[mark,space,...]} or {"ok":false,"error":"..."}.
    std::string LearnFrame(int rx_gpio, int timeout_ms);

    // Send a raw mark/space microsecond pattern on tx_gpio (NEC-style).
    bool SendRaw(int tx_gpio, const uint16_t* marks_spaces_us, size_t count);

    // Send a NEC frame (addr, cmd; addr/cmd are 8-bit).
    bool SendNec(int tx_gpio, uint8_t addr, uint8_t cmd);

    // TX a frame on tx_gpio while capturing on rx_gpio. Returns RX JSON.
    // Used to verify the TX path independently of the AC unit.
    std::string Loopback(int tx_gpio, int rx_gpio, const uint16_t* marks_spaces_us,
                         size_t count, int timeout_ms = 2000);

private:
    IrService() = default;
    // Serializes RMT channel create/destroy so learn/send can't interleave.
    std::mutex rmt_mutex_;
};
