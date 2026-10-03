"""Exercise the real camera pause/resume methods with transient V4L2 failures."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def resume_harness(source):
    methods = source[source.index('bool EspVideo::PauseStream()'):
                     source.index('bool EspVideo::SetHMirror(')]
    return r'''
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

constexpr int V4L2_BUF_TYPE_VIDEO_CAPTURE = 1;
constexpr int V4L2_MEMORY_MMAP = 2;
constexpr int VIDIOC_QBUF = 3;
constexpr int VIDIOC_STREAMON = 4;
constexpr int VIDIOC_STREAMOFF = 5;
struct v4l2_buffer {
    int type = 0;
    int memory = 0;
    uint32_t index = 0;
};

bool running = true;
bool queued[2] = {true, true};
int queue_successes[2] = {};
int streamon_calls = 0;
int streamoff_calls = 0;
int fail_qbuf_once_at = -1;
bool fail_streamon_once = false;
bool fail_streamoff_once = false;

int ioctl(int, int request, void* arg) {
    if (request == VIDIOC_STREAMOFF) {
        ++streamoff_calls;
        if (fail_streamoff_once) {
            fail_streamoff_once = false;
            errno = EIO;
            return -1;
        }
        if (!running) {
            errno = EINVAL;
            return -1;
        }
        running = false;
        queued[0] = queued[1] = false;
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        auto index = static_cast<v4l2_buffer*>(arg)->index;
        if (index >= 2 || queued[index] || running) {
            errno = EINVAL;
            return -1;
        }
        if (fail_qbuf_once_at == static_cast<int>(index)) {
            fail_qbuf_once_at = -1;
            errno = EIO;
            return -1;
        }
        queued[index] = true;
        ++queue_successes[index];
        return 0;
    }
    if (request == VIDIOC_STREAMON) {
        ++streamon_calls;
        if (fail_streamon_once) {
            fail_streamon_once = false;
            errno = EIO;
            return -1;
        }
        if (running || !queued[0] || !queued[1]) {
            errno = EINVAL;
            return -1;
        }
        running = true;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

class EspVideo {
public:
    std::mutex capture_mutex_;
    std::atomic_bool streaming_on_{true};
    bool stream_paused_ = false;
    size_t resume_next_buffer_ = 0;
    int video_fd_ = 1;
    std::vector<int> mmap_buffers_ = {0, 1};
    bool PauseStream();
    bool ResumeStream();
};
''' + methods + r'''
int main() {
    EspVideo camera;

    // A failed STREAMOFF must preserve the active stream and its queue state.
    fail_streamoff_once = true;
    if (camera.PauseStream() || !camera.streaming_on_ || camera.stream_paused_ ||
        !running || camera.resume_next_buffer_ != 0) return 1;

    // After STREAMOFF, the second QBUF fails once. Retry must not QBUF index 0 again.
    if (!camera.PauseStream() || running || camera.resume_next_buffer_ != 0) return 2;
    fail_qbuf_once_at = 1;
    if (camera.ResumeStream() || !camera.stream_paused_ || camera.streaming_on_ ||
        running || camera.resume_next_buffer_ != 1 || queue_successes[0] != 1 ||
        queue_successes[1] != 0 || streamon_calls != 0) return 3;
    if (!camera.ResumeStream() || !running || camera.stream_paused_ ||
        !camera.streaming_on_ || camera.resume_next_buffer_ != 0 ||
        queue_successes[0] != 1 || queue_successes[1] != 1) return 4;

    // If STREAMON fails, both buffers remain queued. Retry only STREAMON.
    if (!camera.PauseStream()) return 5;
    fail_streamon_once = true;
    if (camera.ResumeStream() || !camera.stream_paused_ || camera.streaming_on_ ||
        running || camera.resume_next_buffer_ != 2 || queue_successes[0] != 2 ||
        queue_successes[1] != 2) return 6;
    if (!camera.PauseStream() || camera.resume_next_buffer_ != 2) return 7;
    if (!camera.ResumeStream() || !running || camera.stream_paused_ ||
        !camera.streaming_on_ || camera.resume_next_buffer_ != 0 ||
        queue_successes[0] != 2 || queue_successes[1] != 2) return 8;

    // A later pause must reset queue progress for a fresh start.
    if (!camera.PauseStream() || !camera.ResumeStream() ||
        queue_successes[0] != 3 || queue_successes[1] != 3 ||
        streamon_calls != 4 || streamoff_calls != 4) return 9;
    std::puts("QBUF and STREAMON retries preserve the V4L2 queue state.");
}
'''


class CameraResumeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_transient_qbuf_and_streamon_failures_retry_without_duplicate_queue(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/boards/common/esp_video.cc').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'camera-resume.cc'
            binary = Path(directory) / 'camera-resume'
            test.write_text(resume_harness(source))
            subprocess.run(['c++', '-std=c++17', '-pthread', str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
