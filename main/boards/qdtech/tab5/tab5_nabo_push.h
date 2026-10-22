#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// NABO 每日推送: one-way pushes from the NAS relay (/nabo/<token>/inbox), kept on the
// SD card with the user's own read/done/learned/favourite marks. 往日回顾 is built here
// from that local history; the relay never sends it. See docs/nabo-daily-push.md.
namespace tab5_nabo {

enum class Kind { Fortune, Paper, Word, Point, Todo, Other };

struct Item {
    std::string id, ts, source, priority, mood, title, body, time;
    int rev = 0;
    Kind kind() const;
    std::string Day() const { return ts.size() >= 10 ? ts.substr(0, 10) : std::string(); }
    bool high() const { return priority == "high"; }
};

// The parsed nabo/fortune body. Missing modules keep present=false and show 「暂无」.
struct Fortune {
    bool ok = false;
    int score = 0;
    std::string summary, color, nabo, number;
    std::vector<std::string> yi, ji;
    std::vector<std::pair<std::string, int>> bars;
    struct Chenggu { bool present = false; std::string weight, plain; std::vector<std::string> verse; } chenggu;
    struct Zhouyi {
        bool present = false;
        std::string gua, bian, plain;
        int gua_lines[6] = {}, bian_lines[6] = {};  // bottom to top, 1 = yang
        std::vector<int> moving;                     // 1..6 from the bottom
    } zhouyi;
    struct Lingqian { bool present = false; std::string no, level, explain; std::vector<std::string> poem; } lingqian;
    struct Card { std::string pos, name, meaning; bool upright = true; };
    std::vector<Card> tarot;
    struct Huangli {
        bool present = false;
        std::string lunar, ganzhi, zhishen, jianchu;
        std::vector<std::string> yi, ji;
    } huangli;
    struct Xingzuo { bool present = false; std::string sign, line; std::vector<std::pair<std::string, int>> index; } xingzuo;
    struct Ziwei { bool present = false; std::string main, palace, comment; } ziwei;
    struct Qimen { bool present = false; std::string good, bad, advice; } qimen;
};
Fortune ParseFortune(const std::string& body);

// Minutes after midnight for a to-do ("HH:MM " title prefix or the relay's time field);
// untimed items sort last.
int TodoMinutes(const Item& item);
std::string TodoLabel(const Item& item);  // title without the time prefix

// A sorted list of keys: what std::set offered here, for a fraction of the code size.
class KeySet {
public:
    bool count(const std::string& key) const;
    bool insert(const std::string& key);  // true when added
    bool erase(const std::string& key);   // true when removed
    template <typename Keep>
    size_t KeepIf(Keep keep) {
        const size_t before = keys_.size();
        size_t out = 0;
        for (size_t i = 0; i < keys_.size(); ++i)
            if (keep(keys_[i]))
                keys_[out++] = std::move(keys_[i]);
        keys_.resize(out);
        return before - out;
    }
    const std::vector<std::string>& keys() const { return keys_; }

private:
    std::vector<std::string> keys_;
};

struct Marks {
    KeySet read, done, learned, fav, fav_days;
};

struct DaySummary {
    std::string day;  // YYYY-MM-DD
    int score = 0;
    std::string fortune;
    std::vector<std::string> study;
    int study_total = 0, learned = 0;
    std::vector<std::string> undone;
    int todo_total = 0, done = 0;
    bool fav = false;
};

// Immutable view handed to the UI; rebuilt after every change.
struct Data {
    std::vector<Item> items;  // revision order
    Marks marks;
    uint32_t version = 0;
    bool ever_synced = false;
    bool online = false;

    const Item* TodayFortune(const std::string& today) const;
    std::vector<const Item*> TodayStudy(const std::string& today) const;  // newest first
    std::vector<const Item*> TodayTodos(const std::string& today) const;  // by time
    std::vector<DaySummary> Review(const std::string& today) const;      // newest day first
    int Unread(const std::string& today) const;
};
using SharedData = std::shared_ptr<const Data>;

enum class Mark { Read, Done, Learned, Fav, FavDay };

class Store {
public:
    static Store& GetInstance();
    // Loads the SD history once; safe to call repeatedly.
    void Load();
    // Applies one /nabo inbox page. Returns true when it changed anything; *more is set
    // when the relay has another page waiting.
    bool ApplyInbox(const std::string& body, bool* more);
    void SetOnline(bool online);
    int Since();
    SharedData Get();
    void SetMark(Mark mark, const std::string& key, bool value);
    void MarkAllRead(const std::string& today);
    // Drops history older than kRetainDays before today; no-op while the clock is unset.
    void Prune(const std::string& today);

    static constexpr int kRetainDays = 30;

    // Host tests: start from an empty store that never touches the SD card.
    void ResetForTest();

private:
    Store() = default;
    void PublishLocked();
    void SaveLocked();

    std::mutex mutex_;
    bool loaded_ = false;
    bool persist_ = true;
    int since_ = 0;
    std::vector<Item> items_;
    Marks marks_;
    uint32_t version_ = 0;
    bool ever_synced_ = false, online_ = false;
    SharedData shared_;
};

// Local calendar day for the device clock, or "" before SNTP has set it.
std::string Today();
std::string ShiftDay(const std::string& day, int days);

}  // namespace tab5_nabo
