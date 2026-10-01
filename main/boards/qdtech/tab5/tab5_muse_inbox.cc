#include "tab5_muse_inbox.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <memory>

#include "board.h"
#include "settings.h"
#include "application.h"

#include <cstdlib>

namespace tab5_muse {
namespace {

const char* TAG = "Tab5Muse";
constexpr const char* kDefaultHost = "192.168.3.200:8787";
constexpr size_t kMaxJsonBytes = 32 * 1024;
constexpr int kTimeoutMs = 4000;
constexpr int kPollSeconds = 120;
constexpr int kFirstPollDelayMs = 20000;  // let Wi-Fi and the XiaoZhi session settle first

std::string HttpGet(const std::string& url, int* status_out) {
    *status_out = 0;
    auto network = Board::GetInstance().GetNetwork();
    if (!network)
        return {};
    auto http = network->CreateHttp(0);
    if (!http)
        return {};
    http->SetTimeout(kTimeoutMs);
    http->SetHeader("Accept", "application/json");
    if (!http->Open("GET", url))
        return {};
    const auto status = http->GetStatusCode();
    *status_out = status ? *status : 0;
    std::string body;
    if (*status_out == 200 && http->GetBodyLength() <= kMaxJsonBytes) {
        char buffer[1024];
        while (body.size() <= kMaxJsonBytes) {
            auto read = http->Read(buffer, sizeof(buffer));
            if (!read || *read == 0)
                break;
            if (body.size() + *read > kMaxJsonBytes) {
                body.clear();
                break;
            }
            body.append(buffer, *read);
        }
    }
    http->Close();
    return body;
}

std::string JsonString(cJSON* object, const char* key) {
    auto* item = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// The poll task runs on a PSRAM stack, and NVS writes touch flash (cache disabled), which
// asserts on such a stack. Persist through the main task instead.
void SaveLater(const char* key, const std::string& value) {
    Application::GetInstance().Schedule([key = std::string(key), value] {
        Settings settings("museinbox", true);
        if (key == "seen")
            settings.SetInt(key, std::atoi(value.c_str()));
        else
            settings.SetString(key, value);
    });
}

}  // namespace

Inbox& Inbox::GetInstance() {
    static Inbox instance;
    return instance;
}

void Inbox::Start(Listener listener) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_)
            return;
        listener_ = std::move(listener);
        Settings settings("museinbox", false);
        snapshot_.host = settings.GetString("host", kDefaultHost);
        snapshot_.url = settings.GetString("url", "");
        topic_ = settings.GetString("topic", "");
        snapshot_.seen_id = settings.GetInt("seen", 0);
    }
    // HTTP + cJSON parsing need a roomy stack; keep it out of internal SRAM.
    if (xTaskCreatePinnedToCoreWithCaps(TaskEntry, "muse_inbox", 8192, this, 2, &task_, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "task create failed");
    }
}

void Inbox::RequestRefresh() {
    if (task_)
        xTaskNotifyGive(task_);
}

void Inbox::MarkAllSeen() {
    Snapshot copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.seen_id == snapshot_.latest_id)
            return;
        snapshot_.seen_id = snapshot_.latest_id;
        Settings settings("museinbox", true);
        settings.SetInt("seen", snapshot_.seen_id);
        copy = snapshot_;
    }
    if (listener_)
        listener_(copy);
}

Snapshot Inbox::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void Inbox::SetHost(const std::string& host) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.host = host.empty() ? kDefaultHost : host;
        Settings settings("museinbox", true);
        settings.SetString("host", snapshot_.host);
    }
    RequestRefresh();
}

bool Inbox::SetUrl(const std::string& value) {
    std::string url = value;
    while (!url.empty() && (url.back() == '/' || url.back() == ' ' || url.back() == '\n'))
        url.pop_back();
    const auto mcp = url.find("/mcp/");
    if (mcp != std::string::npos)
        url.replace(mcp, 5, "/inbox/");
    if (url.rfind("https://", 0) != 0 || url.find("/inbox/") == std::string::npos)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.url = url;
        failures_ = 0;
        Settings settings("museinbox", true);
        settings.SetString("url", url);
    }
    ESP_LOGI(TAG, "inbox url set (%u chars)", unsigned(url.size()));
    RequestRefresh();
    return true;
}

// The relay publishes its current quick-tunnel hostname to an ntfy.sh topic it told us
// about earlier. Look it up and, when it changed, rewrite the stored URL.
bool Inbox::Discover() {
    std::string topic, url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        topic = topic_;
        url = snapshot_.url;
    }
    const auto path = url.find("/inbox/");
    if (topic.empty() || path == std::string::npos)
        return false;
    int status = 0;
    const std::string body =
        HttpGet("https://ntfy.sh/" + topic + "/json?poll=1&since=latest", &status);
    // Newline-delimited JSON; the last "message" event holds the hostname.
    std::string host;
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos)
            end = body.size();
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> line(
            cJSON_ParseWithLength(body.data() + start, end - start), cJSON_Delete);
        if (line && JsonString(line.get(), "event") == "message")
            host = JsonString(line.get(), "message");
        start = end + 1;
    }
    if (host.empty() || host.find('/') != std::string::npos || host.size() > 120) {
        ESP_LOGW(TAG, "discovery: no host (http %d)", status);
        return false;
    }
    const std::string fresh = "https://" + host + url.substr(path);
    if (fresh == url)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.url = fresh;
        SaveLater("url", fresh);
    }
    ESP_LOGI(TAG, "discovery: relay moved to %s", host.c_str());
    return true;
}

void Inbox::TaskEntry(void* arg) {
    static_cast<Inbox*>(arg)->Run();
}

void Inbox::Run() {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFirstPollDelayMs));
    while (true) {
        Poll();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollSeconds * 1000));
    }
}

bool Inbox::Poll() {
    std::string host, url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host = snapshot_.host;
        url = snapshot_.url;
    }
    const std::string endpoint = url.empty() ? "http://" + host + "/tab5/inbox" : url;
    int status = 0;
    const std::string body = HttpGet(endpoint + "?since=0&limit=12", &status);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        body.empty() ? nullptr : cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    auto* list = root ? cJSON_GetObjectItem(root.get(), "messages") : nullptr;
    const bool ok = cJSON_IsArray(list);

    Snapshot copy;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) {
            changed = snapshot_.ok;
            snapshot_.ok = false;
            ++failures_;
            ESP_LOGW(TAG, "poll %s failed (http %d, %u bytes)", url.empty() ? host.c_str() : "tunnel",
                     status, unsigned(body.size()));
        } else {
            std::vector<Message> messages;
            cJSON* item = nullptr;
            cJSON_ArrayForEach (item, list) {
                auto* id = cJSON_GetObjectItem(item, "id");
                if (!cJSON_IsNumber(id))
                    continue;
                Message m;
                m.id = id->valueint;
                m.title = JsonString(item, "title");
                m.body = JsonString(item, "body");
                m.from = JsonString(item, "from");
                m.time = JsonString(item, "time");
                messages.push_back(std::move(m));
            }
            auto* latest = cJSON_GetObjectItem(root.get(), "latest_id");
            const int latest_id = cJSON_IsNumber(latest) ? latest->valueint : 0;
            std::string mcp_url = JsonString(root.get(), "mcp_url");
            changed = !snapshot_.ok || latest_id != snapshot_.latest_id ||
                      messages.size() != snapshot_.messages.size() ||
                      mcp_url != snapshot_.mcp_url;
            if (latest_id < snapshot_.seen_id) {
                // The relay was reset (fresh data folder): start counting unread again.
                snapshot_.seen_id = 0;
                SaveLater("seen", "0");
            }
            failures_ = 0;
            const std::string topic = JsonString(root.get(), "discovery_topic");
            if (!topic.empty() && topic != topic_) {
                topic_ = topic;
                SaveLater("topic", topic_);
            }
            snapshot_.ok = true;
            snapshot_.ever_ok = true;
            snapshot_.latest_id = latest_id;
            snapshot_.mcp_url = std::move(mcp_url);
            snapshot_.messages = std::move(messages);
            if (changed)
                ESP_LOGI(TAG, "inbox latest=%d unread=%d", latest_id, snapshot_.Unread());
        }
        copy = snapshot_;
    }
    if (changed && listener_)
        listener_(copy);
    // Two failures in a row through the tunnel: the relay may have a new hostname.
    if (!ok && !url.empty() && failures_ >= 2 && Discover())
        return Poll();
    return ok;
}

}  // namespace tab5_muse
