#include "websocket_control_server.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <sys/param.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "mcp_server.h"
#include "wifi_manager.h"

static const char* TAG = "WSControl";
static constexpr size_t kMaxFrameBytes = 2048;
static constexpr int kMaxJsonDepth = 8;

static bool HasReasonableJsonDepth(const char* data, size_t len) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len; ++i) {
        const char ch = data[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
        } else if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > kMaxJsonDepth) {
                return false;
            }
        } else if (ch == '}' || ch == ']') {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0 && !in_string;
}

WebSocketControlServer* WebSocketControlServer::instance_ = nullptr;

WebSocketControlServer::WebSocketControlServer() : server_handle_(nullptr) { instance_ = this; }

WebSocketControlServer::~WebSocketControlServer() {
    Stop();
    instance_ = nullptr;
}

esp_err_t WebSocketControlServer::ws_handler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }

    // This local endpoint is used by the daily brief sender.  The setup AP is
    // open to nearby devices, so never expose the endpoint during provisioning.
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsConnected() || wifi.IsConfigMode()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        const int sock_fd = httpd_req_to_sockfd(req);
        instance_->RemoveClient(sock_fd);
        instance_->AddClient(req);
        return ESP_OK;
    }

    // Some ESP-IDF httpd builds invoke the handler only for data frames and
    // complete the handshake internally, so register lazily here as well.
    instance_->AddClient(req);

    httpd_ws_frame_t ws_pkt;
    uint8_t* buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        instance_->RemoveClient(req);
        return ret;
    }
    if (ws_pkt.len > kMaxFrameBytes) {
        ESP_LOGW(TAG, "Rejecting WebSocket frame of %zu bytes", ws_pkt.len);
        instance_->RemoveClient(req);
        return ESP_ERR_INVALID_SIZE;
    }

    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            instance_->RemoveClient(req);
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            instance_->RemoveClient(req);
            free(buf);
            return ret;
        }
    }

    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket close frame received");
        instance_->RemoveClient(req);
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        if (ws_pkt.len > 0 && buf != nullptr) {
            buf[ws_pkt.len] = '\0';
            instance_->HandleMessage(req, (const char*)buf, ws_pkt.len);
        }
    } else {
        ESP_LOGW(TAG, "Unsupported frame type: %d", ws_pkt.type);
    }

    free(buf);
    return ESP_OK;
}

bool WebSocketControlServer::Start(int port) {
    if (server_handle_) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.ctrl_port = 32769;
    config.close_fn = OnSessionClosed;

    httpd_uri_t ws_uri = {.uri = "/ws",
                          .method = HTTP_GET,
                          .handler = ws_handler,
                          .user_ctx = nullptr,
                          .is_websocket = true};

    if (httpd_start(&server_handle_, &config) == ESP_OK) {
        if (httpd_register_uri_handler(server_handle_, &ws_uri) != ESP_OK) {
            httpd_stop(server_handle_);
            server_handle_ = nullptr;
            ESP_LOGE(TAG, "Failed to register WebSocket URI");
            return false;
        }
        ESP_LOGI(TAG, "WebSocket server started on port %d", port);
        return true;
    }

    ESP_LOGE(TAG, "Failed to start WebSocket server");
    return false;
}

void WebSocketControlServer::Stop() {
    httpd_handle_t server = nullptr;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        server = server_handle_;
        server_handle_ = nullptr;
        clients_.clear();
    }
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
}

void WebSocketControlServer::HandleMessage(httpd_req_t* req, const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        ESP_LOGE(TAG, "Invalid message: data is null or len is 0");
        return;
    }

    if (len > kMaxFrameBytes || !HasReasonableJsonDepth(data, len)) {
        ESP_LOGW(TAG, "Rejected oversized or overly nested local message: %zu bytes", len);
        return;
    }

    cJSON* root = cJSON_ParseWithLength(data, len);

    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    int sock_fd = httpd_req_to_sockfd(req);
    uint64_t client_id = GetClientId(req);
    if (client_id == 0) {
        ESP_LOGW(TAG, "Ignoring message from unknown client: %d", sock_fd);
        cJSON_Delete(root);
        return;
    }

    auto response_sender = [sock_fd, client_id](const std::string& payload) {
        if (WebSocketControlServer::instance_) {
            WebSocketControlServer::instance_->SendMessage(sock_fd, client_id, payload);
        }
    };

    // The LAN sender only needs this one tool.  Never route arbitrary MCP
    // messages to the global server; it also exposes device-control tools.
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    const cJSON* payload = cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0
                               ? cJSON_GetObjectItem(root, "payload")
                               : root;
    const cJSON* method = cJSON_GetObjectItem(payload, "method");
    const cJSON* params = cJSON_GetObjectItem(payload, "params");
    const cJSON* name = cJSON_GetObjectItem(params, "name");
    if (cJSON_IsObject(payload) && cJSON_IsString(method) &&
        strcmp(method->valuestring, "tools/call") == 0 && cJSON_IsObject(params) &&
        cJSON_IsString(name) && strcmp(name->valuestring, "self.daily.set_cards") == 0) {
        McpServer::GetInstance().ParseMessage(payload, response_sender);
    } else {
        ESP_LOGW(TAG, "Rejected unsupported local MCP request fd=%d", sock_fd);
        const cJSON* id = cJSON_GetObjectItem(payload, "id");
        if (cJSON_IsNumber(id)) {
            response_sender("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id->valueint) +
                            ",\"error\":{\"code\":-32601,\"message\":\"Local endpoint only accepts "
                            "daily cards\"}}");
        }
    }

    cJSON_Delete(root);
}

void WebSocketControlServer::AddClient(httpd_req_t* req) {
    int sock_fd = httpd_req_to_sockfd(req);
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (clients_.find(sock_fd) == clients_.end()) {
        uint64_t client_id = ++next_client_id_;
        clients_[sock_fd] = ClientInfo{.client_id = client_id};
        ESP_LOGI(TAG, "Client connected: %d id=%llu (total: %zu)", sock_fd,
                 (unsigned long long)client_id, clients_.size());
    }
}

void WebSocketControlServer::RemoveClient(httpd_req_t* req) {
    RemoveClient(httpd_req_to_sockfd(req));
}

void WebSocketControlServer::RemoveClient(int sock_fd) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(sock_fd);
    ESP_LOGI(TAG, "Client disconnected: %d (total: %zu)", sock_fd, clients_.size());
}

void WebSocketControlServer::OnSessionClosed(httpd_handle_t server, int sock_fd) {
    (void)server;
    if (instance_) {
        instance_->RemoveClient(sock_fd);
    }
    close(sock_fd);
}

size_t WebSocketControlServer::GetClientCount() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return clients_.size();
}

uint64_t WebSocketControlServer::GetClientId(httpd_req_t* req) const {
    int sock_fd = httpd_req_to_sockfd(req);
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(sock_fd);
    if (it == clients_.end()) {
        return 0;
    }
    return it->second.client_id;
}

struct WsBroadcastJob {
    httpd_handle_t server;
    int fd;
    char* payload;
    size_t len;
};

static void ws_broadcast_send_job(void* arg) {
    WsBroadcastJob* job = static_cast<WsBroadcastJob*>(arg);

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = reinterpret_cast<uint8_t*>(job->payload);
    ws_pkt.len = job->len;
    ws_pkt.final = true;

    esp_err_t ret = httpd_ws_send_frame_async(job->server, job->fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE("WSControl", "BroadcastMessage: send failed fd=%d err=%d", job->fd, ret);
    }

    free(job->payload);
    free(job);
}

void WebSocketControlServer::BroadcastMessage(const std::string& message) {
    std::vector<std::pair<int, uint64_t>> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        if (!server_handle_) {
            return;
        }
        for (const auto& [fd, client] : clients_) {
            clients.emplace_back(fd, client.client_id);
        }
    }
    for (const auto& [fd, id] : clients) {
        SendMessage(fd, id, message);
    }
}

void WebSocketControlServer::SendMessage(int sock_fd, uint64_t client_id,
                                         const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (!server_handle_) {
        return;
    }

    auto client = clients_.find(sock_fd);
    if (client == clients_.end() || client->second.client_id != client_id) {
        ESP_LOGW(TAG, "Skip send to stale client fd=%d id=%llu", sock_fd,
                 (unsigned long long)client_id);
        return;
    }

    WsBroadcastJob* job = static_cast<WsBroadcastJob*>(malloc(sizeof(WsBroadcastJob)));
    if (!job) {
        ESP_LOGE(TAG, "SendMessage: failed to allocate job");
        return;
    }

    job->server = server_handle_;
    job->fd = sock_fd;
    job->len = message.length();
    job->payload = static_cast<char*>(malloc(message.length() + 1));
    if (!job->payload) {
        ESP_LOGE(TAG, "SendMessage: failed to allocate payload");
        free(job);
        return;
    }
    memcpy(job->payload, message.c_str(), message.length());
    job->payload[message.length()] = '\0';

    esp_err_t ret = httpd_queue_work(server_handle_, ws_broadcast_send_job, job);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SendMessage: httpd_queue_work failed fd=%d err=%d", sock_fd, ret);
        free(job->payload);
        free(job);
    }
}
