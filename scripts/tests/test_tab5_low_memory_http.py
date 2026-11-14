"""Host checks for the internal-heap connect gate and the no-throw HTTP error path.

The captured panic was abort() in __cxa_allocate_exception after std::mutex::lock
built a system_error. These cases compile the helpers the firmware uses.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / "vendor_overrides/78__esp-ml307"

SMOKE = r'''
#include "heap_policy.h"
#include "nothrow_lock.h"
#include "network_error.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>

static bool count_new = false;
static int new_calls = 0;

void* operator new(size_t n) {
    if (count_new)
        ++new_calls;
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return std::malloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

int main() {
    assert(!HeapAllowsNewConnection(7275, 3840));
    assert(!HeapAllowsNewConnection(139263, 3840));
    assert(!HeapAllowsNewConnection(32 * 1024 - 1, 8 * 1024));
    assert(HeapAllowsNewConnection(32 * 1024, 8 * 1024));
    assert(HeapAllowsNewConnection(139263, 98304));

    pthread_mutex_t mutex;
    pthread_mutexattr_t attr;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
    assert(pthread_mutex_init(&mutex, &attr) == 0);
    assert(LockIfAvailable(&mutex));
    assert(!LockIfAvailable(&mutex));
    UnlockIfHeld(&mutex, true);
    assert(LockIfAvailable(&mutex));
    UnlockIfHeld(&mutex, true);
    assert(!LockIfAvailable(nullptr));
    pthread_mutex_destroy(&mutex);
    pthread_mutexattr_destroy(&attr);

    char buf[96];
    count_new = true;
    new_calls = 0;
    const char* text = NetworkError::TlsFailed(32791).Format(buf, sizeof(buf));
    count_new = false;
    assert(new_calls == 0);
    assert(std::strcmp(text, "TLS handshake failed (tls_failed, native=32791)") == 0);
    assert(std::strcmp(NetworkError::NetworkUnavailable().Format(buf, sizeof(buf)),
                       "Network unavailable (network_unavailable)") == 0);
    std::puts("low memory http ok");
    return 0;
}
'''

STUB_ESP_ERR = r'''
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define ESP_FAIL -1
inline const char* esp_err_to_name(esp_err_t) { return "ESP_ERR_STUB"; }
'''


@unittest.skipUnless(shutil.which("c++"), "Host C++ compiler required")
class LowMemoryHttpTests(unittest.TestCase):
    def test_gate_failed_lock_and_error_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            temp = Path(tmp)
            (temp / "esp_err.h").write_text(STUB_ESP_ERR)
            (temp / "smoke.cc").write_text(SMOKE)
            subprocess.run(
                ["c++", "-std=c++23", "-Wall", "-Wextra", "-Werror", "-I", str(temp),
                 "-I", str(VENDOR / "include"), str(temp / "smoke.cc"),
                 str(VENDOR / "src/network_error.cc"), "-o", str(temp / "smoke")],
                check=True)
            result = subprocess.run([str(temp / "smoke")], check=True, capture_output=True, text=True)
            self.assertIn("low memory http ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
