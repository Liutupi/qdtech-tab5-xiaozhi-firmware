"""Exercise Tab5's real output and jack routing code with codec/I2C fakes."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


def run_harness(source):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "headphone.cc"
        binary = Path(directory) / "headphone"
        path.write_text(source)
        subprocess.run(["c++", "-std=c++23", "-pthread", str(path), "-o", str(binary)],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)


@unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
class Tab5HeadphoneTests(unittest.TestCase):
    def test_both_ears_chunk_boundaries_reopen_and_write_recovery(self):
        source = (BOARD / "tab5_audio_codec.cc").read_text()
        reopen = source[source.index("void Tab5AudioCodec::ReopenOutputLocked()"):
                        source.index("void Tab5AudioCodec::ReopenOutput()")]
        write = source[source.index("int Tab5AudioCodec::Write("):]
        run_harness(r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(n) (1U << (n))
constexpr int ESP_OK = 0;
struct esp_codec_dev_sample_info_t {
    int bits_per_sample, channel, channel_mask;
    uint32_t sample_rate;
    int mclk_multiple;
};
std::vector<int16_t> written;
std::vector<esp_codec_dev_sample_info_t> formats;
int failed_writes = 0, failed_opens = 0, reopens = 0;
int esp_codec_dev_close(int) { return 0; }
int esp_codec_dev_open(int device, esp_codec_dev_sample_info_t* fs) {
    formats.push_back(*fs);
    if (device == 1) ++reopens;
    if (failed_opens) { --failed_opens; return -1; }
    return 0;
}
int esp_codec_dev_set_out_vol(int, int) { return 0; }
int esp_codec_dev_set_in_channel_gain(int, unsigned, float) { return 0; }
int esp_codec_dev_write(int, void* data, int bytes) {
    assert(bytes > 0 && bytes <= 1024 && bytes % 4 == 0);
    if (failed_writes) { --failed_writes; return -1; }
    auto* pcm = static_cast<int16_t*>(data);
    written.insert(written.end(), pcm, pcm + bytes / 2);
    return 0;
}
struct AudioCodec {
    bool input_enabled_ = false, output_enabled_ = true;
    void EnableInput(bool b) { input_enabled_ = b; }
    void EnableOutput(bool b) { output_enabled_ = b; }
};
struct Tab5AudioCodec : AudioCodec {
    int output_dev_ = 1, input_dev_ = 2, output_sample_rate_ = 24000, output_volume_ = 30;
    float input_gain_ = 30;
    bool input_reference_ = true;
    unsigned write_failures_ = 0;
    std::mutex codec_mutex_;
    std::array<int16_t, 512> stereo_output_{};
    void ReopenOutputLocked();
    int Write(const int16_t*, int);
};
''' + reopen + write + r'''
int main() {
    Tab5AudioCodec codec;
    for (int length : {1, 255, 256, 257, 513, 1152, 4097}) {
        std::vector<int16_t> pcm(length);
        for (int i = 0; i < length; ++i) pcm[i] = static_cast<int16_t>(i * 997);
        const auto original = pcm;
        written.clear();
        assert(codec.Write(pcm.data(), length) == length);
        assert(written.size() == pcm.size() * 2);
        for (int i = 0; i < length; ++i) {
            assert(written[2*i] == pcm[i] && written[2*i+1] == pcm[i]);
        }
        assert(pcm == original);
    }
    written.clear();
    int16_t pcm[] = {-32768, 0, 32767};
    assert(codec.Write(nullptr, 1) == 0);
    assert(codec.Write(pcm, 0) == 0);
    assert(codec.Write(pcm, -1) == 0);
    codec.output_enabled_ = false;
    codec.Write(pcm, 3);
    assert(written.empty());
    codec.output_enabled_ = true;
    codec.input_enabled_ = true;
    codec.ReopenOutputLocked();
    assert(formats[0].channel == 2 && formats[0].channel_mask == 0);
    assert(formats[1].channel == 4 && formats[1].channel_mask == 3);
    assert(codec.input_enabled_ && codec.output_enabled_);
    const int before = reopens;
    failed_writes = 4;
    for (int i = 0; i < 4; ++i) codec.Write(pcm, 3);
    assert(reopens == before + 1 && codec.write_failures_ == 0);
    codec.Write(pcm, 3);
    assert((written == std::vector<int16_t>{-32768,-32768,0,0,32767,32767}));
    failed_opens = 1;
    codec.ReopenOutputLocked();
    assert(!codec.output_enabled_ && codec.input_enabled_);
}
''')

    def test_jack_boot_debounce_unplug_io_failure_and_other_pins(self):
        source = (BOARD / "m5stack_tab5.cc").read_text()
        registers = source[source.index("// PI4IO registers"):
                           source.index("class Pi4ioe1")]
        expander = source[source.index("class Pi4ioe1"):
                          source.index("class Pi4ioe2")]
        run_harness(r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#define ESP_LOGI(...) ((void)0)
using i2c_master_bus_handle_t = void*;
using esp_err_t = int;
constexpr int ESP_OK = 0;
std::array<uint8_t, 256> regs{};
int fail_reg = -1, failed_writes = 0, writes = 0;
int i2c_master_transmit_receive(int, const uint8_t* reg, int, uint8_t* out, int, int) {
    if (*reg == fail_reg) return -1;
    *out = regs[*reg];
    return 0;
}
int i2c_master_transmit(int, const uint8_t* data, int, int) {
    if (failed_writes) { --failed_writes; return -1; }
    regs[data[0]] = data[1];
    ++writes;
    return 0;
}
struct I2cDevice {
    int i2c_device_ = 1;
    I2cDevice(void*, uint8_t) {}
    void WriteReg(uint8_t r, uint8_t v) { regs[r] = v; }
    uint8_t ReadReg(uint8_t r) { return regs[r]; }
};
''' + registers + expander + r'''
int main() {
    // Boot with the transmitter already inserted must never enable the speaker.
    Pi4ioe1 plugged(nullptr, 0x43);
    const uint8_t other_pins = regs[PI4IO_REG_OUT_SET];
    regs[PI4IO_REG_IN_STA] = 0x80;
    for (int i = 0; i < 4; ++i) {
        assert(plugged.PollHeadphone() == ESP_OK);
        assert(regs[PI4IO_REG_OUT_SET] == other_pins);
    }
    // A brief unplug or failed sample must not turn the speaker on.
    regs[PI4IO_REG_IN_STA] = 0;
    plugged.PollHeadphone();
    plugged.PollHeadphone();
    assert(regs[PI4IO_REG_OUT_SET] == other_pins);
    fail_reg = PI4IO_REG_IN_STA;
    assert(plugged.PollHeadphone() != ESP_OK);
    fail_reg = -1;
    plugged.PollHeadphone();
    plugged.PollHeadphone();
    assert(regs[PI4IO_REG_OUT_SET] == other_pins);
    plugged.PollHeadphone();
    assert(regs[PI4IO_REG_OUT_SET] == (other_pins | 2));
    // Write/read errors retry routing without committing a false state.
    regs[PI4IO_REG_IN_STA] = 0x80;
    plugged.PollHeadphone();
    plugged.PollHeadphone();
    failed_writes = 1;
    assert(plugged.PollHeadphone() != ESP_OK);
    assert(regs[PI4IO_REG_OUT_SET] & 2);
    fail_reg = PI4IO_REG_OUT_SET;
    assert(plugged.PollHeadphone() != ESP_OK);
    fail_reg = -1;
    assert(plugged.PollHeadphone() == ESP_OK);
    assert(regs[PI4IO_REG_OUT_SET] == other_pins);
    const int before = writes;
    for (int i = 0; i < 100; ++i) plugged.PollHeadphone();
    assert(writes == before);
    // Concurrent updates to independent output bits must preserve SPK_EN and
    // camera/display reset lines sharing the same output register.
    std::thread a([&] { for (int i=0;i<1000;++i) plugged.UpdateOutputBit(0, i%2); });
    std::thread b([&] { for (int i=0;i<1000;++i) plugged.UpdateOutputBit(2, i%2); });
    a.join(); b.join();
    assert(regs[PI4IO_REG_OUT_SET] == (other_pins | 1 | 4));
    assert(!(regs[PI4IO_REG_OUT_SET] & 2));
    Pi4ioe1 unplugged(nullptr, 0x43);
    regs[PI4IO_REG_IN_STA] = 0;
    for (int i = 0; i < 3; ++i) unplugged.PollHeadphone();
    assert(regs[PI4IO_REG_OUT_SET] == (other_pins | 2));
}
''')


if __name__ == "__main__":
    unittest.main()
