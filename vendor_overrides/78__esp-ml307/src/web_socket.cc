#include "web_socket.h"
#include <esp_log.h>
#include <esp_pthread.h>
#include <sdkconfig.h>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include "network_interface.h"

#define TAG "WebSocket"

namespace {
constexpr size_t kMaxHandshakeBytes = 8 * 1024;
#if CONFIG_SPIRAM_USE_MALLOC
// Glyph push permits 64 KiB of bitmap data, or about 88 KiB after base64.
// These large std::string/std::vector allocations use PSRAM in malloc mode.
constexpr size_t kMaxMessageBytes = 128 * 1024;
#else
// Parsing temporarily holds both the wire frame and its accumulated message.
// Keep each below 16 KiB when std::string/std::vector use internal RAM.
constexpr size_t kMaxMessageBytes = 16 * 1024;
#endif
// ESP TCP delivers 1500-byte chunks. Leave room for the final chunk and frame
// header without allowing another message-sized allocation in the receive buffer.
constexpr size_t kMaxBufferedBytes = kMaxMessageBytes + 2 * 1024;

struct WebSocketUrl {
    std::string protocol;
    std::string host;
    std::string path;
    int port = 0;
    bool tls = false;
};

bool ParseWebSocketUrl(const char* uri, WebSocketUrl& parsed) {
    if (uri == nullptr)
        return false;
    const std::string_view url(uri);
    for (const unsigned char byte : url) {
        // The path is written directly into an HTTP request line below.
        if (byte <= 0x20 || byte == 0x7f)
            return false;
    }
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
        return false;
    const auto scheme = url.substr(0, scheme_end);
    if (scheme != "ws" && scheme != "wss" && scheme != "http" && scheme != "https")
        return false;

    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    const auto authority = url.substr(authority_start, authority_end - authority_start);
    // The ESP TCP implementation is IPv4-only. Do not mistake IPv6 colons or
    // user-info for a port, and never pass malformed authority to DNS.
    if (authority.empty() || authority.find_first_of("@[] \t\r\n") != std::string_view::npos)
        return false;
    const size_t colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty())
        return false;

    parsed.tls = scheme == "wss" || scheme == "https";
    parsed.port = parsed.tls ? 443 : 80;
    if (colon != std::string_view::npos) {
        const auto port_text = authority.substr(colon + 1);
        if (port_text.empty())
            return false;
        int port = 0;
        const auto [end, error] =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (error != std::errc{} || end != port_text.data() + port_text.size() || port < 1 ||
            port > 65535)
            return false;
        parsed.port = port;
    }

    if (authority_end != std::string_view::npos && url[authority_end] == '#')
        return false;
    const auto suffix =
        authority_end == std::string_view::npos ? std::string_view{} : url.substr(authority_end);
    if (suffix.find('#') != std::string_view::npos)
        return false;
    parsed.protocol = scheme;
    parsed.host = host;
    parsed.path = suffix.empty()
                      ? "/"
                      : (suffix.front() == '?' ? "/" + std::string(suffix) : std::string(suffix));
    return true;
}
}  // namespace

static std::string base64_encode(const unsigned char *data, size_t len) {
    const char *base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    size_t i = 0;
    while (i < len) {
        size_t chunk_size = std::min((size_t)3, len - i);

        for (size_t j = 0; j < 3; j++) {
            char_array_3[j] = (j < chunk_size) ? data[i + j] : 0;
        }

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (size_t j = 0; j < 4; j++) {
            if (j <= chunk_size) {
                encoded.push_back(base64_chars[char_array_4[j]]);
            } else {
                encoded.push_back('=');
            }
        }

        i += chunk_size;
    }
    return encoded;
}

WebSocket::WebSocket(NetworkInterface* network, int connect_id)
    : network_(network), connect_id_(connect_id) {
    handshake_event_group_ = xEventGroupCreate();
}

WebSocket::~WebSocket() {
    if (tcp_) {
        // The TCP receive callback uses this object's handshake event group.
        // Join the receive task before deleting either object, even when the
        // peer already disconnected or the handshake never completed.
        tcp_->Disconnect();
        tcp_.reset();
    }
    if (handshake_event_group_) {
        vEventGroupDelete(handshake_event_group_);
    }
}

void WebSocket::SetHeader(const char* key, const char* value) { headers_[key] = value; }

void WebSocket::SetReceiveBufferSize(size_t size) { receive_buffer_size_ = size; }

bool WebSocket::IsConnected() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return connected_;
}

NetworkResult<> WebSocket::Connect(const char* uri) {
    if (interrupted_.load(std::memory_order_acquire)) {
        return Fail(NetworkError::ServerDisconnected());
    }
    WebSocketUrl parsed;
    if (!ParseWebSocketUrl(uri, parsed)) {
        ESP_LOGE(TAG, "Invalid WebSocket URI");
        return Fail(NetworkError::InvalidArgument());
    }

    ESP_LOGD(TAG, "Connecting to %s://%s:%d%s", parsed.protocol.c_str(), parsed.host.c_str(),
             parsed.port, parsed.path.c_str());
    uint32_t t_connect = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 设置 WebSocket 特定的头部
    SetHeader("Upgrade", "websocket");
    SetHeader("Connection", "Upgrade");
    SetHeader("Sec-WebSocket-Version", "13");

    // 生成随机的 Sec-WebSocket-Key
    char key[25];
    for (int i = 0; i < 16; ++i) {
        key[i] = rand() % 256;
    }
    std::string base64_key = base64_encode(reinterpret_cast<const unsigned char*>(key), 16);
    SetHeader("Sec-WebSocket-Key", base64_key.c_str());

    std::unique_ptr<Tcp> previous;
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        previous = std::move(tcp_);
    }
    if (previous) {
        previous->Disconnect();
        previous.reset();
    }
    std::unique_ptr<Tcp> created;
    if (parsed.tls) {
        created = network_->CreateSsl(connect_id_);
    } else {
        created = network_->CreateTcp(connect_id_);
    }

    if (!created) {
        return Fail(NetworkError::NotInitialized());
    }
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        if (interrupted_.load(std::memory_order_acquire)) {
            return Fail(NetworkError::ServerDisconnected());
        }
        tcp_ = std::move(created);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connected_ = false;
        handshake_pending_ = true;
        handshake_failed_ = false;
    }
    handshake_completed_ = false;
    receive_buffer_.clear();
    // Clear and install callbacks before Connect starts its receive task.
    xEventGroupClearBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT);
    tcp_->OnStream([this](const std::string& data) { this->OnTcpData(data); });
    tcp_->OnDisconnected([this]() { HandleTcpDisconnected(); });
    // 使用 tcp 建立连接
    if (auto result = tcp_->Connect(parsed.host, parsed.port); !result) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        handshake_pending_ = false;
        ESP_LOGE(TAG, "Failed to connect to server: %s", result.error().ToString().c_str());
        return Fail(result.error());
    }
    if (interrupted_.load(std::memory_order_acquire)) {
        return Fail(NetworkError::ServerDisconnected());
    }
    uint32_t t_connected = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // 发送 WebSocket 握手请求
    std::string request = "GET " + parsed.path + " HTTP/1.1\r\n";
    if (headers_.find("Host") == headers_.end()) {
        request += "Host: " + parsed.host + "\r\n";
    }
    for (const auto& header : headers_) {
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "\r\n";

    if (interrupted_.load(std::memory_order_acquire) || tcp_->Send(request) < 0) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        handshake_pending_ = false;
        ESP_LOGE(TAG, "Failed to send WebSocket handshake request");
        return Fail(NetworkError::TransmitFailed());
    }

    // 等待握手完成，超时时间10秒
    EventBits_t bits =
        xEventGroupWaitBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT,
                            pdFALSE,              // 不清除事件位
                            pdFALSE,              // 等待任意一个事件位
                            pdMS_TO_TICKS(10000)  // 10秒超时
        );
    uint32_t t_handshaked = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Keep the committed state and its callback ordered with any concurrent
    // TCP disconnect callback. That callback may already be waiting here.
    std::unique_lock<std::mutex> callback_lock(callback_mutex_);
    bool connected = false;
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if ((bits & HANDSHAKE_SUCCESS_BIT) && !(bits & HANDSHAKE_FAILED_BIT) &&
            !handshake_failed_ && !interrupted_.load(std::memory_order_acquire) &&
            tcp_->connected()) {
            connected_ = true;
            connected = true;
        }
        failed = (bits & HANDSHAKE_FAILED_BIT) || handshake_failed_ ||
                 ((bits & HANDSHAKE_SUCCESS_BIT) && !connected);
        handshake_pending_ = false;
    }

    if (connected) {
        ESP_LOGI(TAG, "WebSocket handshake done, cost=%u tcp=%u handshake=%u",
                 t_handshaked - t_connect, t_connected - t_connect, t_handshaked - t_connected);
        if (on_connected_) {
            on_connected_();
        }
        return {};
    }
    callback_lock.unlock();
    if (failed) {
        ESP_LOGE(TAG, "WebSocket handshake failed");
        auto err = NetworkError::ProtocolError();
        if (on_error_) {
            on_error_(err);
        }
        return Fail(err);
    } else {
        ESP_LOGE(TAG, "WebSocket handshake timeout");
        return Fail(NetworkError::Timeout());
    }
}

bool WebSocket::Send(const std::string& data) { return Send(data.data(), data.size(), false); }

bool WebSocket::Send(const void* data, size_t len, bool binary, bool fin) {
    if (interrupted_.load(std::memory_order_acquire)) {
        return false;
    }
    if (len > 65535) {
        ESP_LOGE(TAG, "Data too large, maximum supported size is 65535 bytes");
        return false;
    }

    // Data frames and protocol control frames can originate from different
    // tasks. Protect the continuation state along with the actual socket send.
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (interrupted_.load(std::memory_order_acquire) || tcp_ == nullptr) {
        return false;
    }

    std::string frame;
    frame.reserve(len + 8);  // 最大可能的帧大小（2字节帧头 + 2字节长度 + 4字节mask）

    // 第一个字节：FIN 位 + 操作码
    uint8_t first_byte = (fin ? 0x80 : 0x00);
    if (binary) {
        first_byte |= 0x02;  // 二进制帧
    } else if (!continuation_) {
        first_byte |= 0x01;  // 文本帧
    }  // 否则，操作码为0（延续帧）

    frame.push_back(static_cast<char>(first_byte));

    // 第二个字节：MASK 位 + 有效载荷长度
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));  // 设置MASK位
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));  // 设置MASK位
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    }

    // 生成随机的4字节mask
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = rand() & 0xFF;
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);

    // 添加并mask处理有效载荷
    const uint8_t* payload = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }

    // 发送帧
    if (tcp_->Send(frame) != static_cast<int>(frame.size()))
        return false;
    continuation_ = !fin;
    return true;
}

void WebSocket::Ping() { SendControlFrame(0x9, nullptr, 0); }

void WebSocket::Close() {
    if (IsConnected()) {
        SendControlFrame(0x8, nullptr, 0);
    }
}

void WebSocket::Interrupt() {
    interrupted_.store(true, std::memory_order_release);
    // Connect only holds this lock while replacing tcp_. Transport Interrupt
    // performs socket shutdown without waiting for Send or receive callbacks.
    std::lock_guard<std::mutex> lock(tcp_mutex_);
    if (tcp_) {
        tcp_->Interrupt();
    }
}

void WebSocket::Abort() {
    // A sender may hold send_mutex_ while blocked inside Tcp::Send. Tcp::Disconnect
    // shuts down the socket before it waits for its own send lock, releasing that
    // sender without trying to acquire the WebSocket send lock here.
    Interrupt();
    if (tcp_) {
        tcp_->Disconnect();
    }
}

void WebSocket::OnConnected(std::function<void()> callback) { on_connected_ = callback; }

void WebSocket::OnDisconnected(std::function<void()> callback) { on_disconnected_ = callback; }

void WebSocket::OnData(std::function<void(const char*, size_t, bool binary)> callback) {
    on_data_ = callback;
}

void WebSocket::OnError(std::function<void(const NetworkError& error)> callback) {
    on_error_ = callback;
}

void WebSocket::OnPong(std::function<void(const char*, size_t)> callback) { on_pong_ = callback; }

void WebSocket::HandleTcpDisconnected() {
    std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    bool fail_handshake = false;
    bool notify_disconnected = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (handshake_pending_) {
            handshake_failed_ = true;
            fail_handshake = true;
        }
        if (connected_) {
            connected_ = false;
            notify_disconnected = true;
        }
    }
    if (fail_handshake) {
        xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
    }
    if (notify_disconnected && on_disconnected_) {
        on_disconnected_();
    }
}

void WebSocket::RejectIncomingData(const char* reason) {
    ESP_LOGE(TAG, "Rejecting WebSocket data: %s", reason);
    // A rejected peer must not retain a large partial frame until the owner
    // eventually destroys this WebSocket. Shutdown wakes the receive task;
    // its normal disconnect callback then notifies the protocol.
    std::string().swap(receive_buffer_);
    std::vector<char>().swap(current_message_);
    is_fragmented_ = false;
    is_binary_ = false;
    Interrupt();
}

void WebSocket::OnTcpData(const std::string& data) {
    if (interrupted_.load(std::memory_order_acquire))
        return;
    if (data.size() > kMaxBufferedBytes ||
        receive_buffer_.size() > kMaxBufferedBytes - data.size()) {
        RejectIncomingData("receive buffer limit exceeded");
        return;
    }
    receive_buffer_.append(data);

    if (!handshake_completed_) {
        size_t pos = receive_buffer_.find("\r\n\r\n");
        if (pos != std::string::npos) {
            if (pos + 4 > kMaxHandshakeBytes) {
                RejectIncomingData("handshake header limit exceeded");
                return;
            }
            std::string handshake_response = receive_buffer_.substr(0, pos + 4);
            receive_buffer_ = receive_buffer_.substr(pos + 4);

            if (handshake_response.find("HTTP/1.1 101") != std::string::npos) {
                handshake_completed_ = true;
                // 设置握手成功事件
                xEventGroupSetBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT);
            } else {
                ESP_LOGE(TAG, "WebSocket handshake failed");
                // 设置握手失败事件
                xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
                return;
            }
        } else {
            if (receive_buffer_.size() > kMaxHandshakeBytes)
                RejectIncomingData("handshake header limit exceeded");
            return;
        }
    }

    size_t buffer_offset = 0;
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(receive_buffer_.data());
    size_t buffer_size = receive_buffer_.size();

    while (buffer_offset < buffer_size) {
        if (interrupted_.load(std::memory_order_acquire)) {
            std::string().swap(receive_buffer_);
            std::vector<char>().swap(current_message_);
            return;
        }
        if (buffer_size - buffer_offset < 2)
            break;  // 需要更多数据

        uint8_t opcode = buffer[buffer_offset] & 0x0F;
        bool fin = (buffer[buffer_offset] & 0x80) != 0;
        uint8_t mask = buffer[buffer_offset + 1] & 0x80;
        uint64_t payload_length = buffer[buffer_offset + 1] & 0x7F;

        size_t header_length = 2;
        if (payload_length == 126) {
            if (buffer_size - buffer_offset < 4)
                break;  // 需要更多数据
            payload_length =
                (static_cast<uint64_t>(buffer[buffer_offset + 2]) << 8) | buffer[buffer_offset + 3];
            header_length += 2;
        } else if (payload_length == 127) {
            if (buffer_size - buffer_offset < 10)
                break;  // 需要更多数据
            payload_length = 0;
            for (int i = 0; i < 8; ++i) {
                payload_length = (payload_length << 8) | buffer[buffer_offset + 2 + i];
            }
            header_length += 8;
        }

        const bool data_frame = opcode == 0x0 || opcode == 0x1 || opcode == 0x2;
        if (data_frame) {
            if ((opcode == 0x0 && !is_fragmented_) || (opcode != 0x0 && is_fragmented_)) {
                RejectIncomingData("invalid frame continuation");
                return;
            }
            const size_t existing_bytes = opcode == 0x0 ? current_message_.size() : 0;
            if (payload_length > kMaxMessageBytes - existing_bytes) {
                RejectIncomingData("message limit exceeded");
                return;
            }
        } else if (!fin || payload_length > 125) {
            RejectIncomingData("invalid control frame");
            return;
        }

        uint8_t mask_key[4] = {0};
        if (mask) {
            if (buffer_size - buffer_offset < header_length + 4)
                break;  // 需要更多数据
            memcpy(mask_key, buffer + buffer_offset + header_length, 4);
            header_length += 4;
        }

        if (buffer_size - buffer_offset < header_length + payload_length)
            break;  // 需要更多数据
        const size_t frame_size = static_cast<size_t>(payload_length);
        const char* payload = reinterpret_cast<const char*>(buffer + buffer_offset + header_length);

        switch (opcode) {
            case 0x0:  // 延续帧
            case 0x1:  // 文本帧
            case 0x2:  // 二进制帧
                // Server frames are normally unmasked and unfragmented. The
                // callback consumes data synchronously, so deliver directly
                // from receive_buffer_ instead of allocating a second large
                // buffer for every text or audio frame.
                if (opcode != 0x0 && fin && !mask) {
                    if (on_data_)
                        on_data_(payload, frame_size, opcode == 0x2);
                    break;
                }
                if (opcode != 0x0) {
                    is_fragmented_ = !fin;
                    is_binary_ = (opcode == 0x2);
                    current_message_.clear();
                }
                {
                    const size_t old_size = current_message_.size();
                    current_message_.insert(current_message_.end(), payload, payload + frame_size);
                    if (mask) {
                        for (size_t i = 0; i < frame_size; ++i)
                            current_message_[old_size + i] ^= mask_key[i % 4];
                    }
                }
                if (fin) {
                    if (on_data_) {
                        on_data_(current_message_.data(), current_message_.size(), is_binary_);
                    }
                    if (current_message_.capacity() > 32 * 1024)
                        std::vector<char>().swap(current_message_);
                    else
                        current_message_.clear();
                    is_fragmented_ = false;
                }
                break;
            case 0x8:  // 关闭帧
                std::string().swap(receive_buffer_);
                std::vector<char>().swap(current_message_);
                Interrupt();
                return;
            case 0x9:  // Ping
            {
                char control_payload[125];
                if (frame_size != 0)
                    memcpy(control_payload, payload, frame_size);
                if (mask) {
                    for (size_t i = 0; i < frame_size; ++i)
                        control_payload[i] ^= mask_key[i % 4];
                }
                SendControlFrame(0xA, control_payload, frame_size);
            } break;
            case 0xA:  // Pong
                if (on_pong_) {
                    char control_payload[125];
                    if (frame_size != 0)
                        memcpy(control_payload, payload, frame_size);
                    if (mask) {
                        for (size_t i = 0; i < frame_size; ++i)
                            control_payload[i] ^= mask_key[i % 4];
                    }
                    on_pong_(control_payload, frame_size);
                }
                break;
            default:
                RejectIncomingData("unknown frame opcode");
                return;
        }

        buffer_offset += header_length + payload_length;
    }

    // 保留未处理的数据
    if (buffer_offset == buffer_size) {
        std::string().swap(receive_buffer_);
    } else if (buffer_offset > 0) {
        receive_buffer_ = receive_buffer_.substr(buffer_offset);
    }
}

bool WebSocket::SendControlFrame(uint8_t opcode, const void* data, size_t len) {
    if (interrupted_.load(std::memory_order_acquire) || len > 125) {
        ESP_LOGE(TAG, "控制帧有效载荷过大");
        return false;
    }

    std::string frame;
    frame.reserve(len + 6);  // 帧头 + 掩码 + 有效载荷

    // 第一个字节：FIN 位 + 操作码
    frame.push_back(static_cast<char>(0x80 | opcode));

    // 第二个字节：MASK 位 + 有效载荷长度
    frame.push_back(static_cast<char>(0x80 | len));

    // 生成随机的4字节掩码
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = rand() & 0xFF;
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);

    // 添加并掩码处理有效载荷
    const uint8_t* payload = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }

    // 发送帧
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (interrupted_.load(std::memory_order_acquire) || tcp_ == nullptr) {
        return false;
    }
    return tcp_->Send(frame) == static_cast<int>(frame.size());
}
