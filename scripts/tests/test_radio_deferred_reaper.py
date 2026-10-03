"""Exercise RadioService's actual deferred-reader ownership handoff."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/radio_service.cc"


def make_harness(source: str) -> str:
    begin = source.index("struct DeferredReader {")
    end = source.index("}  // namespace", begin) + len("}  // namespace")
    lifecycle = source[begin:end]
    return r'''
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TAG "RadioService"
#define pdMS_TO_TICKS(ms) (ms)
constexpr int pdTRUE = 1, pdPASS = 1, pdFALSE = 0;
constexpr int portMAX_DELAY = -1;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
using TickType_t = int;
using BaseType_t = int;
using TaskHandle_t = void*;
struct Semaphore { std::atomic<bool> signalled{false}; };
using SemaphoreHandle_t = Semaphore*;
struct StaticStreamBuffer_t {};
struct Ring { uint8_t* storage; StaticStreamBuffer_t* control; };
using StreamBufferHandle_t = Ring*;
struct Client { int id; };
using esp_http_client_handle_t = Client*;
struct StreamReader { StreamBufferHandle_t ring; SemaphoreHandle_t done; };
struct Queue {
    size_t capacity;
    size_t item_size;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<uint8_t>> items;
};
using QueueHandle_t = Queue*;

std::atomic<bool> fail_task_create{false};
std::atomic<bool> expect_async{false};
std::thread::id main_thread;
std::mutex calls_mutex;
std::array<int, 12> close_calls{}, cleanup_calls{};
std::atomic<int> deleted_tasks{0};

int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
    return sem->signalled.exchange(false) ? pdTRUE : pdFALSE;
}
void vSemaphoreDelete(SemaphoreHandle_t sem) { delete sem; }
int xStreamBufferGetStaticBuffers(StreamBufferHandle_t ring, uint8_t** storage,
                                  StaticStreamBuffer_t** control) {
    *storage = ring->storage;
    *control = ring->control;
    return pdTRUE;
}
void vStreamBufferDelete(StreamBufferHandle_t ring) { delete ring; }
void heap_caps_free(void* pointer) { std::free(pointer); }
void vTaskDeleteWithCaps(TaskHandle_t task) {
    ++deleted_tasks;
    delete static_cast<int*>(task);
}
void esp_http_client_close(esp_http_client_handle_t client) {
    if (expect_async && std::this_thread::get_id() == main_thread) std::abort();
    std::lock_guard<std::mutex> lock(calls_mutex);
    ++close_calls[client->id];
}
void esp_http_client_cleanup(esp_http_client_handle_t client) {
    {
        std::lock_guard<std::mutex> lock(calls_mutex);
        ++cleanup_calls[client->id];
    }
    delete client;
}
QueueHandle_t xQueueCreate(size_t capacity, size_t item_size) {
    return new Queue{capacity, item_size};
}
void vQueueDelete(QueueHandle_t queue) { delete queue; }
int xQueueSend(QueueHandle_t queue, const void* item, TickType_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.size() == queue->capacity) return pdFALSE;
    std::vector<uint8_t> copy(queue->item_size);
    std::memcpy(copy.data(), item, queue->item_size);
    queue->items.push_back(std::move(copy));
    queue->ready.notify_one();
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    if (wait == portMAX_DELAY) {
        queue->ready.wait(lock, [&] { return !queue->items.empty(); });
    } else if (!queue->ready.wait_for(lock, std::chrono::milliseconds(wait),
                                      [&] { return !queue->items.empty(); })) {
        return pdFALSE;
    }
    std::memcpy(item, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return pdTRUE;
}
void vTaskDelay(TickType_t ticks) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*entry)(void*), const char*,
                                            uint32_t, void* arg, int, TaskHandle_t* task,
                                            int, int) {
    if (fail_task_create) return pdFALSE;
    *task = new int(1);  // Firmware keeps its one reaper task for the process lifetime.
    std::thread(entry, arg).detach();
    return pdPASS;
}

namespace {
''' + lifecycle + r'''

DeferredReader new_orphan(int id) {
    auto* reader = new StreamReader;
    reader->ring = new Ring{static_cast<uint8_t*>(std::malloc(32)),
                            static_cast<StaticStreamBuffer_t*>(std::malloc(sizeof(StaticStreamBuffer_t)))};
    reader->done = new Semaphore;
    return {reader, new int(id), new Client{id}};
}
bool wait_for_outstanding(size_t expected) {
    for (int i = 0; i < 200; ++i) {
        if (g_deferred_reader_count.load() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
bool cleaned_once(int id) {
    std::lock_guard<std::mutex> lock(calls_mutex);
    return close_calls[id] == 1 && cleanup_calls[id] == 1;
}
int main() {
    main_thread = std::this_thread::get_id();
    // If worker startup fails, only the player task owns the fallback slot.
    fail_task_create = true;
    auto fallback = new_orphan(0);
    if (!DeferStreamReader(fallback) || g_fallback_reader_count != 1 ||
        g_deferred_reader_count.load() != 1) return 1;
    fallback.reader->done->signalled = true;
    ReapFallbackReaders();
    if (g_fallback_reader_count != 0 || g_deferred_reader_count.load() != 0 ||
        !cleaned_once(0)) return 2;

    fail_task_create = false;
    expect_async = true;
    auto first = new_orphan(1);
    auto second = new_orphan(2);
    auto third = new_orphan(3);
    if (!DeferStreamReader(first) || !DeferStreamReader(second) ||
        !DeferStreamReader(third)) return 3;
    if (g_deferred_reader_count.load() != 3 || g_fallback_reader_count != 0) return 4;
    second.reader->done->signalled = true;
    if (!wait_for_outstanding(2) || !cleaned_once(2)) return 5;
    {
        std::lock_guard<std::mutex> lock(calls_mutex);
        if (cleanup_calls[1] || cleanup_calls[3]) return 6;
    }
    first.reader->done->signalled = true;
    third.reader->done->signalled = true;
    if (!wait_for_outstanding(0) || !cleaned_once(1) || !cleaned_once(3)) return 7;

    std::array<DeferredReader, 4> full;
    for (int i = 0; i < 4; ++i) {
        full[i] = new_orphan(4 + i);
        if (!DeferStreamReader(full[i])) return 8;
    }
    if (DeferStreamReader(full[0]) || g_deferred_reader_count.load() != 4) return 9;
    full[3].reader->done->signalled = true;
    if (!wait_for_outstanding(3)) return 10;
    auto replacement = new_orphan(8);
    if (!DeferStreamReader(replacement)) return 11;
    for (int i = 0; i < 3; ++i) full[i].reader->done->signalled = true;
    replacement.reader->done->signalled = true;
    if (!wait_for_outstanding(0)) return 12;
    for (int i = 4; i <= 8; ++i) if (!cleaned_once(i)) return 13;
    if (deleted_tasks != 9) return 14;
    std::_Exit(0);  // The firmware reaper intentionally has process lifetime.
}
'''


class RadioDeferredReaperTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_completed_orphans_reclaimed_async_out_of_order_and_capped(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "radio_reaper.cc"
            binary = Path(directory) / "radio_reaper"
            cpp.write_text(make_harness(SOURCE.read_text()))
            subprocess.run(["c++", "-std=c++17", "-pthread", str(cpp), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_audio_decode_loop_does_not_run_synchronous_cleanup(self):
        source = SOURCE.read_text()
        loop = source[source.index("    while (play_requested_ && !stop_requested_ && WifiManager"):
                      source.index("    const bool reader_stopped = stop_reader();")]
        self.assertNotIn("ReapFallbackReaders()", loop)
        self.assertNotIn("esp_http_client_cleanup", loop)


if __name__ == "__main__":
    unittest.main()
