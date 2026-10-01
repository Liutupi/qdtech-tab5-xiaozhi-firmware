#pragma once
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// IR receive (learning) and transmit through the ESP32-P4 RMT peripheral.
//
// Timings are stored as alternating mark/space durations in microseconds, starting with a
// mark: [mark, space, mark, space, ...]. A value may be up to 65535 us, so the gap between
// the parts of a multi-frame air-conditioner code is kept.
class IrService {
public:
    static IrService& GetInstance() {
        static IrService instance;
        return instance;
    }

    // Learn one button press on rx_gpio. Waits up to timeout_ms for the first frame, then keeps
    // listening briefly for follow-up frames (multi-part AC codes). NEC repeat codes and exact
    // repeats of an already captured frame are dropped. Returns false with a Chinese reason.
    bool Learn(int rx_gpio, int timeout_ms, std::vector<uint16_t>* timings, std::string* error,
               const std::atomic<bool>* cancel = nullptr);

    // Transmit timings on tx_gpio with a carrier. active_high selects the LED drive polarity.
    bool Send(int tx_gpio, const uint16_t* timings, size_t count, bool active_high = true,
              uint32_t carrier_hz = 38000, int repeat = 1);

    // Transmit a test frame on tx_gpio while listening on rx_gpio. True when the receiver
    // heard it (the module's own receiver usually sees its LED). Used to find the polarity.
    bool LoopbackHeard(int tx_gpio, int rx_gpio, bool active_high);

    // Count edges on each GPIO for `seconds`. Returns JSON: {"gpio":count,...}
    std::string ScanGpios(const int* gpios, size_t n, int seconds);

    // JSON wrappers kept for the MCP debug tools.
    std::string LearnFrame(int rx_gpio, int timeout_ms);
    bool SendRaw(int tx_gpio, const uint16_t* marks_spaces_us, size_t count) {
        return Send(tx_gpio, marks_spaces_us, count, true, 38000, 1);
    }

    static std::string TimingsToJson(const std::vector<uint16_t>& timings);

private:
    IrService() = default;
    // Serializes RMT channel create/destroy so learn/send can't interleave.
    std::mutex rmt_mutex_;
};
