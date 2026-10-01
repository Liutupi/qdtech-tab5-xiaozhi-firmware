#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Polls the NAS "Muse inbox" relay over the LAN. Muse (Meta's cloud agent) pushes short
// messages to the relay through a public MCP endpoint; the Tab5 only ever talks to the NAS.
namespace tab5_muse {

struct Message {
    int id = 0;
    std::string title;
    std::string body;
    std::string from;
    std::string time;
};

struct Snapshot {
    bool ok = false;          // last poll succeeded
    bool ever_ok = false;     // at least one poll succeeded since boot
    int latest_id = 0;
    int seen_id = 0;          // highest id the user has viewed
    std::string host;         // NAS host:port (LAN) — used only when no url is set
    std::string url;          // public inbox URL https://<tunnel>/inbox/<token>
    std::string mcp_url;      // public MCP URL (only when the tunnel is up)
    std::vector<Message> messages;  // newest first

    int Unread() const {
        int n = 0;
        for (const auto& m : messages)
            n += m.id > seen_id;
        return n;
    }
};

class Inbox {
public:
    using Listener = std::function<void(const Snapshot&)>;

    static Inbox& GetInstance();
    void Start(Listener listener);
    void RequestRefresh();
    void MarkAllSeen();
    Snapshot Current();
    void SetHost(const std::string& host);
    // Accepts the Muse MCP URL (.../mcp/<token>) or the inbox URL (.../inbox/<token>).
    bool SetUrl(const std::string& url);

private:
    Inbox() = default;
    static void TaskEntry(void* arg);
    void Run();
    bool Poll();
    bool Discover();
    int failures_ = 0;
    std::string topic_;

    std::mutex mutex_;
    Snapshot snapshot_;
    Listener listener_;
    TaskHandle_t task_ = nullptr;
};

}  // namespace tab5_muse
