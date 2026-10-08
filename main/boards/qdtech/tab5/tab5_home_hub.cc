#include "tab5_home_hub.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>

#include "application.h"
#include "board.h"
#include "settings.h"
#include "tab5_muse_inbox.h"

namespace tab5_home {
namespace {

const char* TAG = "Tab5Home";
constexpr size_t kMaxCatalogBytes = 24 * 1024;
constexpr size_t kMaxErrorBytes = 512;
constexpr size_t kMaxJobs = 8;
constexpr int kTimeoutMs = 8000;
constexpr int kFirstFetchMs = 25000;          // after Wi-Fi, the inbox and the session
constexpr int kVisiblePollMs = 5000;
constexpr int kIdlePollMs = 10 * 60 * 1000;   // keep the catalog warm for voice commands
constexpr int kAfterControlMs = 1200;         // HA reports the new state shortly after
constexpr int kRetryMs = 60 * 1000;

int64_t NowMs() { return esp_timer_get_time() / 1000; }

struct Response {
    int status = 0;
    tab5_podcast::Str body;
};

Response Request(const char* method, const std::string& url, const std::string& key, std::string body,
                 size_t max_bytes) {
    Response out;
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http)
        return out;
    http->SetTimeout(kTimeoutMs);
    if (!key.empty())
        http->SetHeader("X-Home-Key", key);
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(body));
    }
    if (http->Open(method, url)) {
        const auto status = http->GetStatusCode();
        out.status = status ? *status : 0;
        const size_t limit = out.status == 200 ? max_bytes : kMaxErrorBytes;
        char buffer[512];
        while (true) {
            auto read = http->Read(buffer, sizeof(buffer));
            if (!read || *read <= 0)
                break;
            if (out.body.size() + *read > limit) {
                if (out.status == 200)
                    out.status = -1;  // too large: treat as a failure
                break;
            }
            out.body.append(buffer, *read);
        }
        http->Close();
    }
    return out;
}

// {"ok":false,"error":"ha_unreachable"} -> ha_unreachable
std::string ErrorCode(const Response& r) {
    static constexpr std::string_view kKey = "\"error\":\"";
    std::string_view body(r.body.data(), r.body.size());
    const size_t at = body.find(kKey);
    if (at == std::string_view::npos)
        return {};
    const size_t end = body.find('"', at + kKey.size());
    return end == std::string_view::npos ? std::string() : std::string(body.substr(at + kKey.size(), end - at - kKey.size()));
}

std::string Describe(const Response& r) {
    const std::string code = ErrorCode(r);
    if (r.status == 0)
        return "连不上 NAS 中转站，稍后自动重试";
    if (r.status == 404 && (code.empty() || code == "not_found"))
        return "NAS 中转站还没有升级米家中控功能";
    if (code == "ha_token_missing")
        return "NAS 上还没有 Home Assistant 令牌 (data/ha_token.txt)";
    if (code == "ha_token_rejected")
        return "Home Assistant 拒绝了令牌，请重新生成";
    if (code == "ha_unreachable" || code.rfind("ha_http_", 0) == 0)
        return "Home Assistant 暂时没有响应";
    if (code == "home_key_required")
        return "配对已失效，正在重新配对";
    if (code == "unknown_scene")
        return "没有这个场景";
    return "请求失败 (" + std::to_string(r.status) + (code.empty() ? "" : " " + code) + ")";
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20)
            out += c;
    }
    return out;
}

}  // namespace

Hub& Hub::GetInstance() {
    static Hub instance;
    return instance;
}

void Hub::Start(Listener listener) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_)
            return;
        listener_ = std::move(listener);
        Settings settings("home", false);
        key_ = settings.GetString("key", "");
        status_.paired = !key_.empty();
    }
    // HTTPS through the tunnel needs a roomy stack; keep it out of internal SRAM.
    if (xTaskCreatePinnedToCoreWithCaps(TaskEntry, "home_hub", 8192, this, 2, &task_, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "task create failed");
    }
}

void Hub::SetPageVisible(bool visible) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        visible_ = visible;
        refresh_ = refresh_ || visible;
    }
    if (visible && task_)
        xTaskNotifyGive(task_);
}

void Hub::RequestRefresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_ = true;
    }
    if (task_)
        xTaskNotifyGive(task_);
}

Status Hub::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool Hub::Control(const std::string& entity_id, const std::string& action, const std::string& value,
                  Done done) {
    std::string body = "{\"entity_id\":\"" + JsonEscape(entity_id) + "\",\"action\":\"" + JsonEscape(action) + "\"";
    if (!value.empty())
        body += ",\"value\":\"" + JsonEscape(value) + "\"";
    body += "}";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!task_ || jobs_.size() >= kMaxJobs)
            return false;
        jobs_.push_back({"control", std::move(body), std::move(done)});
    }
    xTaskNotifyGive(task_);
    return true;
}

bool Hub::RunScene(const std::string& scene_id, bool on, Done done) {
    std::string body = "{\"id\":\"" + JsonEscape(scene_id) + "\",\"action\":\"" + (on ? "on" : "off") + "\"}";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!task_ || jobs_.size() >= kMaxJobs)
            return false;
        jobs_.push_back({"scene", std::move(body), std::move(done)});
    }
    xTaskNotifyGive(task_);
    return true;
}

void Hub::TaskEntry(void* arg) { static_cast<Hub*>(arg)->Run(); }

std::string Hub::Base() { return HomeBase(tab5_muse::Inbox::GetInstance().Current().url); }

void Hub::Run() {
    int64_t next_fetch = NowMs() + kFirstFetchMs;
    while (true) {
        const int64_t wait = std::max<int64_t>(0, next_fetch - NowMs());
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<int64_t>(wait, kIdlePollMs)));
        std::vector<Job> jobs;
        bool refresh = false, visible = false;
        std::string key;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs.swap(jobs_);
            refresh = refresh_;
            refresh_ = false;
            visible = visible_;
            key = key_;
        }
        const std::string base = Base();
        if (base.empty()) {
            for (auto& job : jobs)
                if (job.done)
                    job.done(false, "还没有连接 NAS 中转站");
            Publish(false, "还没有连接 NAS 中转站 (在 NABO 每日推送里设置)", nullptr);
            next_fetch = NowMs() + kRetryMs;
            continue;
        }
        if (key.empty() && !Pair(base)) {
            for (auto& job : jobs)
                if (job.done)
                    job.done(false, Current().message);
            next_fetch = NowMs() + (visible ? 15000 : kRetryMs * 5);
            continue;
        }
        for (auto& job : jobs)
            Execute(base, std::move(job));
        if (!jobs.empty())
            next_fetch = std::min(next_fetch, NowMs() + kAfterControlMs);
        if (refresh || NowMs() >= next_fetch) {
            const bool ok = Fetch(base);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                visible = visible_;
            }
            next_fetch = NowMs() + (!ok ? (visible ? 15000 : kRetryMs) : (visible ? kVisiblePollMs : kIdlePollMs));
        }
    }
}

bool Hub::Pair(const std::string& base) {
    Publish(false, "正在与 NAS 配对…", nullptr);
    const Response r = Request("POST", base + "/pair?format=tsv", "", "{}", 256);
    const std::string key = r.status == 200 ? ParsePairKey({r.body.data(), r.body.size()}) : std::string();
    if (key.empty()) {
        const std::string code = ErrorCode(r);
        std::string message = Describe(r);
        if (code == "already_paired")
            message = "NAS 已与其他设备配对：删除 NAS 上 data/home_key.txt 并重启 muse-relay 后重试";
        else if (code == "pairing_closed")
            message = "配对窗口已关闭：重启 NAS 上的 muse-relay，30 分钟内打开米家中控";
        ESP_LOGW(TAG, "pairing failed (http %d %s)", r.status, code.c_str());
        Publish(false, message, nullptr);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        key_ = key;
        status_.paired = true;
    }
    // NVS writes touch flash: never from this PSRAM-stack task.
    Application::GetInstance().Schedule([key] {
        Settings settings("home", true);
        settings.SetString("key", key);
    });
    ESP_LOGI(TAG, "paired with the NAS relay");
    return true;
}

bool Hub::Fetch(const std::string& base) {
    const Response r = Request("GET", base + "/devices?format=tsv", "", "", kMaxCatalogBytes);
    auto catalog = std::allocate_shared<Catalog>(tab5_podcast::PsramAllocator<Catalog>());
    if (r.status != 200 || !ParseCatalog({r.body.data(), r.body.size()}, catalog.get())) {
        ESP_LOGW(TAG, "catalog fetch failed (http %d, %u bytes)", r.status, unsigned(r.body.size()));
        Publish(false, r.status == 200 ? "NAS 返回了无法识别的数据" : Describe(r), nullptr);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.catalog && SameCatalog(*status_.catalog, *catalog))
            catalog.reset();  // unchanged: no redraw
    }
    Publish(true, "", std::move(catalog));
    return true;
}

void Hub::Execute(const std::string& base, Job job) {
    std::string key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        key = key_;
    }
    const Response r = Request("POST", base + "/" + job.path, key, job.body, kMaxErrorBytes);
    const bool ok = r.status == 200;
    std::string message = ok ? std::string() : Describe(r);
    if (r.status == 207)
        message = "部分设备没有响应";
    if (ErrorCode(r) == "home_key_required") {
        // Key revoked on the NAS: forget it and pair again on the next round.
        std::lock_guard<std::mutex> lock(mutex_);
        key_.clear();
        status_.paired = false;
    }
    ESP_LOGI(TAG, "%s -> http %d", job.path.c_str(), r.status);
    if (job.done)
        job.done(ok, message);
}

void Hub::Publish(bool ok, const std::string& message, std::shared_ptr<const Catalog> catalog) {
    Status copy;
    Listener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool changed = status_.ok != ok || status_.message != message || catalog;
        status_.configured = true;
        status_.ok = ok;
        status_.message = message;
        if (catalog)
            status_.catalog = std::move(catalog);
        if (!changed)
            return;
        ++status_.version;
        copy = status_;
        listener = listener_;
    }
    if (listener)
        listener(copy);
}

}  // namespace tab5_home
