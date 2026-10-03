#pragma once

#include <esp_err.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

struct esp_tls_t {
    int fd = -1;
};
struct esp_tls_cfg_t {
    esp_err_t (*crt_bundle_attach)(void*) = nullptr;
    int timeout_ms = 0;
};
using esp_tls_error_handle_t = void*;
constexpr int ESP_TLS_ERR_SSL_WANT_READ = -0x6900;
constexpr int ESP_TLS_ERR_SSL_WANT_WRITE = -0x6880;

inline std::atomic_bool fake_tls_block_write{false};
inline std::atomic_bool fake_tls_want_write{false};
inline std::atomic_bool fake_tls_write_started{false};
inline std::atomic_bool fake_tls_allow_write{false};
inline std::atomic_int fake_tls_destroy_count{0};

inline esp_tls_t* esp_tls_init() { return new esp_tls_t; }

inline int esp_tls_conn_new_sync(const char* host, int host_len, int port, const esp_tls_cfg_t*,
                                 esp_tls_t* tls) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    const std::string address_text(host, static_cast<size_t>(host_len));
    if (inet_pton(AF_INET, address_text.c_str(), &address.sin_addr) != 1)
        return -1;
    tls->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tls->fd < 0)
        return -1;
    if (connect(tls->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        return -1;
    return 1;
}

inline int esp_tls_conn_read(esp_tls_t* tls, void* buffer, size_t len) {
    return static_cast<int>(recv(tls->fd, buffer, len, 0));
}

inline int esp_tls_conn_write(esp_tls_t* tls, const void* buffer, size_t len) {
    if (fake_tls_want_write.load()) {
        fake_tls_write_started = true;
        return ESP_TLS_ERR_SSL_WANT_WRITE;
    }
    if (fake_tls_block_write.load()) {
        fake_tls_write_started = true;
        while (!fake_tls_allow_write.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return static_cast<int>(send(tls->fd, buffer, len, 0));
}

inline esp_err_t esp_tls_get_conn_sockfd(esp_tls_t* tls, int* fd) {
    *fd = tls->fd;
    return tls->fd >= 0 ? ESP_OK : ESP_FAIL;
}

inline esp_err_t esp_tls_get_error_handle(esp_tls_t*, esp_tls_error_handle_t*) { return ESP_FAIL; }

inline esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t, int*, int*) {
    return ESP_FAIL;
}

inline void esp_tls_conn_destroy(esp_tls_t* tls) {
    fake_tls_destroy_count.fetch_add(1);
    if (tls->fd >= 0)
        close(tls->fd);
    delete tls;
}
