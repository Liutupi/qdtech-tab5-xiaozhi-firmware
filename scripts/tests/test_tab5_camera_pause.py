"""Exercise the real camera sampler at a forced pause/lock race boundary."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def camera_harness(source):
    sample = source[source.index('bool EspVideo::CaptureVisionFrame('):
                    source.index('bool EspVideo::ConfigureVisionLowLight(')]
    pause = source[source.index('bool EspVideo::PauseStream()'):
                   source.index('bool EspVideo::ResumeStream()')]
    # Force the actual sampling function to yield immediately before its lock.
    sample = sample.replace('std::lock_guard<std::mutex> capture_lock(capture_mutex_);',
                            'if (before_lock) before_lock();\n'
                            '    std::lock_guard<std::mutex> capture_lock(capture_mutex_);', 1)
    return r'''
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr int V4L2_BUF_TYPE_VIDEO_CAPTURE=1, V4L2_MEMORY_MMAP=2;
constexpr int VIDIOC_DQBUF=3, VIDIOC_QBUF=4, VIDIOC_STREAMOFF=5;
constexpr int V4L2_PIX_FMT_RGB24=6, V4L2_PIX_FMT_RGB565=7;
constexpr int V4L2_PIX_FMT_YUYV=8, V4L2_PIX_FMT_UYVY=9, V4L2_PIX_FMT_YUV422P=10;
struct v4l2_buffer { int type=0, memory=0; unsigned index=0, bytesused=0; };
std::atomic<bool> driver_running{true};
std::atomic<int> stopped_dequeues{0}, active_dequeues{0};
int ioctl(int, int request, void*) {
    if(request==VIDIOC_STREAMOFF) { driver_running=false; return 0; }
    if(request==VIDIOC_DQBUF) {
        if(!driver_running) { ++stopped_dequeues; return -1; }
        ++active_dequeues;
    }
    return 0;
}
class EspVideo {
public:
    struct Buffer { void* start=nullptr; };
    std::mutex capture_mutex_;
    std::atomic<bool> streaming_on_{true};
    bool stream_paused_=false;
    size_t resume_next_buffer_=0;
    int video_fd_=1;
    uint16_t sensor_width_=320, sensor_height_=240;
    int sensor_format_=V4L2_PIX_FMT_RGB24;
    std::vector<Buffer> mmap_buffers_;
    std::function<void()> before_lock;
    bool CaptureVisionFrame(uint8_t*, size_t, uint16_t&, uint16_t&);
    bool PauseStream();
};
''' + sample + pause + r'''
int main() {
    uint8_t rgb[320*240*3]{};
    uint16_t width=0, height=0;
    for(int i=0;i<100;++i) {
        EspVideo camera;
        driver_running=true;
        std::promise<void> at_lock, proceed;
        auto gate=proceed.get_future();
        camera.before_lock=[&]{at_lock.set_value();gate.wait();};
        bool result=true;
        std::thread sampler([&]{result=camera.CaptureVisionFrame(rgb,sizeof(rgb),width,height);});
        at_lock.get_future().wait();
        if(!camera.PauseStream()) return 2;
        proceed.set_value();
        sampler.join();
        if(result || stopped_dequeues) {
            std::fprintf(stderr,"sampler dequeued after PauseStream: %d\n",int(stopped_dequeues));
            return 1;
        }
    }
    EspVideo active;
    driver_running=true;
    active.CaptureVisionFrame(rgb,sizeof(rgb),width,height);
    if(active_dequeues!=1) return 3;
    std::puts("100 forced pause races safe; active stream still dequeues.");
}
'''


class CameraPauseTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_pause_between_state_check_and_lock_never_dequeues_stopped_stream(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/boards/common/esp_video.cc').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'camera-pause.cc'
            binary = Path(directory) / 'camera-pause'
            test.write_text(camera_harness(source))
            subprocess.run(['c++', '-std=c++17', '-pthread', str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
