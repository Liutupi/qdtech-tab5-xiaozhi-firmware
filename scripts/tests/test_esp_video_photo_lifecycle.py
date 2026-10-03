"""Exercise EspVideo's real Explain body with fake JPEG/network dependencies."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/common/esp_video.cc"


def make_harness(source: str) -> str:
    set_url = source[source.index("void EspVideo::SetExplainUrl("):
                     source.index("bool EspVideo::Capture()")]
    explain = source[source.index("std::expected<std::string, std::string> EspVideo::Explain("):]
    return r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define TAG "EspVideo"
constexpr int pdPASS = 1;
constexpr int portMAX_DELAY = -1;
using v4l2_pix_fmt_t = uint32_t;

struct JpegChunk { uint8_t* data; size_t len; };
struct Queue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<JpegChunk> chunks;
};
using QueueHandle_t = Queue*;

enum class Mode {
    Success, QueueFailure, NoNetwork, NoHttp, OpenFailure,
    WriteFailure, EncoderFailure, StatusFailure, BadStatus
};
Mode mode = Mode::Success;
std::mutex allocations_mutex;
std::condition_variable allocations_changed;
std::unordered_set<void*> allocations;
std::function<void()> before_encode;
std::mutex upload_mutex;
std::condition_variable upload_changed;
bool hold_upload = false;
bool upload_entered = false;
uint8_t* allocate(size_t size) {
    auto* result = static_cast<uint8_t*>(std::malloc(size));
    if (!result) std::abort();
    std::lock_guard<std::mutex> lock(allocations_mutex);
    allocations.insert(result);
    return result;
}
void heap_caps_free(void* ptr) {
    if (!ptr) return;
    {
        std::lock_guard<std::mutex> lock(allocations_mutex);
        if (allocations.erase(ptr) != 1) std::abort();
    }
    std::free(ptr);
    allocations_changed.notify_all();
}
void* heap_caps_aligned_alloc(size_t, size_t size, int) { return allocate(size); }
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;

QueueHandle_t xQueueCreate(size_t, size_t) {
    return mode == Mode::QueueFailure ? nullptr : new Queue;
}
int xQueueSend(QueueHandle_t queue, const JpegChunk* chunk, int) {
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->chunks.push_back(*chunk);
    }
    queue->ready.notify_one();
    return pdPASS;
}
int xQueueReceive(QueueHandle_t queue, JpegChunk* chunk, int) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->ready.wait(lock, [&] { return !queue->chunks.empty(); });
    *chunk = queue->chunks.front();
    queue->chunks.pop_front();
    return pdPASS;
}
void vQueueDelete(QueueHandle_t queue) { delete queue; }

using jpg_out_cb = size_t (*)(void*, size_t, const void*, size_t);
bool image_to_jpeg_cb(uint8_t* src, size_t length, uint16_t, uint16_t,
                      v4l2_pix_fmt_t, uint8_t, jpg_out_cb callback, void* arg) {
    if (before_encode) before_encode();
    if (!src || length == 0) std::abort();
    if (mode == Mode::EncoderFailure) return false;
    constexpr char jpeg[] = "JPEG";
    callback(arg, 0, jpeg, sizeof(jpeg) - 1);
    callback(arg, 1, nullptr, 0);
    return true;
}

struct Error { std::string ToString() const { return "mock error"; } };
struct Http {
    int writes = 0;
    void SetHeader(std::string_view, std::string_view) {}
    std::expected<void, Error> Open(std::string_view, std::string_view) {
        if (mode == Mode::OpenFailure) return std::unexpected(Error{});
        return {};
    }
    std::expected<size_t, Error> Write(const char* data, size_t size) {
        if (mode == Mode::WriteFailure && ++writes == 2)
            return std::unexpected(Error{});
        if (size == 4 && std::memcmp(data, "JPEG", 4) == 0) {
            std::unique_lock<std::mutex> lock(upload_mutex);
            if (hold_upload) {
                upload_entered = true;
                upload_changed.notify_all();
                upload_changed.wait(lock, [] { return !hold_upload; });
            }
        }
        return size;
    }
    std::expected<int, Error> GetStatusCode() {
        if (mode == Mode::StatusFailure) return std::unexpected(Error{});
        return mode == Mode::BadStatus ? 503 : 200;
    }
    std::string ReadAll() { return "answer"; }
    void Close() {}
};
struct Network {
    std::unique_ptr<Http> CreateHttp(int) {
        if (mode == Mode::NoHttp) return nullptr;
        return std::make_unique<Http>();
    }
};
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Network* GetNetwork() { return mode == Mode::NoNetwork ? nullptr : &network; }
    std::string GetUuid() { return "uuid"; }
    Network network;
};
struct SystemInfo { static std::string GetMacAddress() { return "mac"; } };
size_t uxTaskGetStackHighWaterMark(void*) { return 4096; }

class EspVideo {
public:
    struct FrameBuffer {
        uint8_t* data = nullptr;
        size_t len = 0;
        uint16_t width = 8;
        uint16_t height = 4;
        v4l2_pix_fmt_t format = 1;
    } frame_;
    std::mutex photo_mutex_;
    std::string explain_url_;
    std::string explain_token_;
    void SetExplainUrl(const std::string&, const std::string&);
    std::expected<std::string, std::string> Explain(const std::string&);
};
''' + set_url + explain + r'''
bool no_live_allocations() {
    std::lock_guard<std::mutex> lock(allocations_mutex);
    return allocations.empty();
}
void capture(EspVideo& camera) {
    std::lock_guard<std::mutex> lock(camera.photo_mutex_);
    if (camera.frame_.data) std::abort();
    camera.frame_.data = allocate(64);
    camera.frame_.len = 64;
    camera.frame_.format = 1;
}
int run_case(Mode scenario, int index, bool set_url = true, bool set_frame = true) {
    mode = scenario;
    EspVideo camera;
    if (set_url) camera.SetExplainUrl("http://local", "token");
    if (set_frame) capture(camera);
    auto result = camera.Explain("question");
    if (bool(result) != (scenario == Mode::Success && set_url && set_frame))
        return index;
    if (result && *result != "answer") return index + 100;
    if (camera.frame_.data || !no_live_allocations()) return index + 200;
    return 0;
}
int main() {
    const Mode scenarios[] = {
        Mode::Success, Mode::QueueFailure, Mode::NoNetwork,
        Mode::NoHttp, Mode::OpenFailure, Mode::WriteFailure,
        Mode::EncoderFailure, Mode::StatusFailure, Mode::BadStatus
    };
    for (int i = 0; i < int(sizeof(scenarios) / sizeof(scenarios[0])); ++i) {
        if (int failure = run_case(scenarios[i], i + 1)) return failure;
    }
    if (int failure = run_case(Mode::Success, 20, false)) return failure;
    if (int failure = run_case(Mode::Success, 21, true, false)) return failure;

    // The encoder releases its source even while the HTTP upload is blocked.
    mode = Mode::Success;
    EspVideo during_upload;
    during_upload.SetExplainUrl("http://local", "token");
    capture(during_upload);
    auto* source = during_upload.frame_.data;
    {
        std::lock_guard<std::mutex> lock(upload_mutex);
        hold_upload = true;
        upload_entered = false;
    }
    bool upload_ok = false;
    std::thread upload([&] { upload_ok = bool(during_upload.Explain("slow")); });
    {
        std::unique_lock<std::mutex> lock(upload_mutex);
        upload_changed.wait(lock, [] { return upload_entered; });
    }
    {
        std::unique_lock<std::mutex> lock(allocations_mutex);
        if (!allocations_changed.wait_for(lock, std::chrono::seconds(2),
                                          [&] { return !allocations.count(source); })) return 30;
    }
    {
        std::lock_guard<std::mutex> lock(upload_mutex);
        hold_upload = false;
    }
    upload_changed.notify_all();
    upload.join();
    if (!upload_ok || !no_live_allocations()) return 31;

    mode = Mode::Success;
    EspVideo camera;
    camera.SetExplainUrl("http://local", "token");
    capture(camera);
    auto* first = camera.frame_.data;
    std::promise<void> encoding_started;
    std::promise<void> continue_encoding;
    auto resume = continue_encoding.get_future();
    before_encode = [&] {
        encoding_started.set_value();
        resume.wait();
    };
    bool first_ok = false;
    std::thread first_explain([&] { first_ok = bool(camera.Explain("first")); });
    encoding_started.get_future().wait();
    capture(camera);  // A new capture may run while old JPEG/network work is pending.
    auto* second = camera.frame_.data;
    if (!second || second == first) return 40;
    continue_encoding.set_value();
    first_explain.join();
    before_encode = nullptr;
    if (!first_ok || camera.frame_.data != second) return 41;
    {
        std::lock_guard<std::mutex> lock(allocations_mutex);
        if (allocations.count(first) || !allocations.count(second)) return 42;
    }
    if (!camera.Explain("second") || !no_live_allocations()) return 43;
    return 0;
}
'''


class EspVideoPhotoLifecycleTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_explain_releases_photo_on_success_failure_and_concurrent_capture(self):
        source = SOURCE.read_text()
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "photo_lifecycle.cc"
            binary = Path(directory) / "photo_lifecycle"
            cpp.write_text(make_harness(source))
            subprocess.run(["c++", "-std=c++23", "-pthread", str(cpp), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_capture_failure_drops_partial_frame(self):
        source = SOURCE.read_text()
        capture = source[source.index("bool EspVideo::Capture()"):
                         source.index("bool EspVideo::CaptureVisionFrame(")]
        self.assertIn("std::lock_guard<std::mutex> photo_lock(photo_mutex_);", capture)
        self.assertIn("CaptureFailureCleanup", capture)
        self.assertIn("cleanup.committed = true;", capture)
        self.assertIn("srm_cfg.out.buffer_size = rotated_size;", capture)
        self.assertNotIn("encoder_thread_", source)


if __name__ == "__main__":
    unittest.main()
