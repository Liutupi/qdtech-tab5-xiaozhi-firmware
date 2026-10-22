#include "tab5_nabo_push.h"

#include <cJSON.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tab5_sd.h"
#endif

namespace tab5_nabo {
namespace {

#ifdef ESP_PLATFORM
const char* TAG = "NaboPush";
constexpr const char* kDir = "/sdcard/tab5/nabo";
constexpr const char* kFile = "/sdcard/tab5/nabo/push.json";
constexpr long kMaxFileBytes = 4 * 1024 * 1024;
#endif
constexpr size_t kMaxItems = 600;

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::string Str(const cJSON* object, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

int Int(const cJSON* object, const char* key, int fallback = 0) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

std::vector<std::string> Strings(const cJSON* object, const char* key, size_t limit = 12) {
    std::vector<std::string> out;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(object, key)) {
        if (cJSON_IsString(item) && item->valuestring && out.size() < limit)
            out.emplace_back(item->valuestring);
    }
    return out;
}

// Object members in their pushed order, e.g. {"事业科研":4,"健康":3}.
std::vector<std::pair<std::string, int>> Pairs(const cJSON* object, const char* key) {
    std::vector<std::pair<std::string, int>> out;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(object, key)) {
        if (cJSON_IsNumber(item) && item->string && out.size() < 8)
            out.emplace_back(item->string, item->valueint);
    }
    return out;
}

void Lines(const cJSON* object, const char* key, int (&out)[6]) {
    const cJSON* item = nullptr;
    int i = 0;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(object, key)) {
        if (i < 6)
            out[i++] = cJSON_IsNumber(item) && item->valueint ? 1 : 0;
    }
}

bool Known(const std::string& source) {
    return source == "nabo/fortune" || source == "nabo/todo" || source == "nabo/study/paper" ||
           source == "nabo/study/word" || source == "nabo/study/point";
}

bool ParseItem(const cJSON* object, Item* out) {
    Item item;
    item.id = Str(object, "id");
    item.source = Str(object, "source");
    item.ts = Str(object, "ts");
    item.title = Str(object, "title");
    if (item.id.empty() || item.id.size() > 80 || !Known(item.source) || item.ts.size() < 10 ||
        item.title.empty())
        return false;
    item.priority = Str(object, "priority");
    item.mood = Str(object, "mood");
    item.body = Str(object, "body");
    item.time = Str(object, "time");
    item.rev = Int(object, "rev");
    *out = std::move(item);
    return true;
}

[[maybe_unused]] cJSON* ItemJson(const Item& item) {
    cJSON* object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "id", item.id.c_str());
    cJSON_AddStringToObject(object, "ts", item.ts.c_str());
    cJSON_AddStringToObject(object, "source", item.source.c_str());
    cJSON_AddStringToObject(object, "priority", item.priority.c_str());
    cJSON_AddStringToObject(object, "mood", item.mood.c_str());
    cJSON_AddStringToObject(object, "title", item.title.c_str());
    cJSON_AddStringToObject(object, "body", item.body.c_str());
    if (!item.time.empty())
        cJSON_AddStringToObject(object, "time", item.time.c_str());
    cJSON_AddNumberToObject(object, "rev", item.rev);
    return object;
}

KeySet* MarkSet(Marks* marks, Mark mark) {
    switch (mark) {
    case Mark::Read: return &marks->read;
    case Mark::Done: return &marks->done;
    case Mark::Learned: return &marks->learned;
    case Mark::Fav: return &marks->fav;
    case Mark::FavDay: return &marks->fav_days;
    }
    return &marks->read;
}

[[maybe_unused]] constexpr const char* kMarkKeys[] = {"read", "done", "learned", "fav", "fav_days"};

}  // namespace

bool KeySet::count(const std::string& key) const {
    return std::binary_search(keys_.begin(), keys_.end(), key);
}

bool KeySet::insert(const std::string& key) {
    const auto at = std::lower_bound(keys_.begin(), keys_.end(), key);
    if (at != keys_.end() && *at == key)
        return false;
    keys_.insert(at, key);
    return true;
}

bool KeySet::erase(const std::string& key) {
    const auto at = std::lower_bound(keys_.begin(), keys_.end(), key);
    if (at == keys_.end() || *at != key)
        return false;
    keys_.erase(at);
    return true;
}

Kind Item::kind() const {
    if (source == "nabo/fortune") return Kind::Fortune;
    if (source == "nabo/todo") return Kind::Todo;
    if (source == "nabo/study/paper") return Kind::Paper;
    if (source == "nabo/study/word") return Kind::Word;
    if (source == "nabo/study/point") return Kind::Point;
    return Kind::Other;
}

Fortune ParseFortune(const std::string& body) {
    Fortune f;
    Json root(cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    if (!cJSON_IsObject(root.get()))
        return f;
    const cJSON* r = root.get();
    f.ok = true;
    f.score = std::clamp(Int(r, "score"), 0, 5);
    f.summary = Str(r, "summary");
    f.yi = Strings(r, "yi", 4);
    f.ji = Strings(r, "ji", 4);
    f.color = Str(r, "color");
    const auto* number = cJSON_GetObjectItemCaseSensitive(r, "number");
    if (cJSON_IsNumber(number))
        f.number = std::to_string(number->valueint);
    else if (cJSON_IsString(number) && number->valuestring)
        f.number = number->valuestring;
    f.bars = Pairs(r, "bars");
    f.nabo = Str(r, "nabo");
    const cJSON* m = cJSON_GetObjectItemCaseSensitive(r, "modules");
    const auto module = [m](const char* key) -> const cJSON* {
        const auto* item = cJSON_GetObjectItemCaseSensitive(m, key);
        return cJSON_IsObject(item) && item->child ? item : nullptr;
    };
    if (const auto* c = module("chenggu")) {
        f.chenggu = {true, Str(c, "weight"), Str(c, "plain"), Strings(c, "verse", 4)};
    }
    if (const auto* z = module("zhouyi")) {
        f.zhouyi.present = true;
        f.zhouyi.gua = Str(z, "gua");
        f.zhouyi.bian = Str(z, "bian");
        f.zhouyi.plain = Str(z, "plain");
        Lines(z, "gua_lines", f.zhouyi.gua_lines);
        Lines(z, "bian_lines", f.zhouyi.bian_lines);
        const cJSON* line = nullptr;
        cJSON_ArrayForEach(line, cJSON_GetObjectItemCaseSensitive(z, "moving")) {
            if (cJSON_IsNumber(line) && line->valueint >= 1 && line->valueint <= 6)
                f.zhouyi.moving.push_back(line->valueint);
        }
    }
    if (const auto* l = module("lingqian")) {
        f.lingqian = {true, Str(l, "no"), Str(l, "level"), Str(l, "explain"), Strings(l, "poem", 4)};
    }
    const cJSON* card = nullptr;
    cJSON_ArrayForEach(card, cJSON_GetObjectItemCaseSensitive(m, "tarot")) {
        if (!cJSON_IsObject(card) || f.tarot.size() >= 3)
            continue;
        const auto* upright = cJSON_GetObjectItemCaseSensitive(card, "upright");
        f.tarot.push_back({Str(card, "pos"), Str(card, "name"), Str(card, "meaning"),
                           !cJSON_IsFalse(upright)});
    }
    if (const auto* h = module("huangli")) {
        f.huangli = {true, Str(h, "lunar"), Str(h, "ganzhi"), Str(h, "zhishen"),
                     Str(h, "jianchu"), Strings(h, "yi", 8), Strings(h, "ji", 8)};
    }
    if (const auto* x = module("xingzuo")) {
        f.xingzuo = {true, Str(x, "sign"), Str(x, "line"), Pairs(x, "index")};
    }
    if (const auto* z = module("ziwei")) {
        f.ziwei = {true, Str(z, "main"), Str(z, "palace"), Str(z, "comment")};
    }
    if (const auto* q = module("qimen")) {
        f.qimen = {true, Str(q, "good"), Str(q, "bad"), Str(q, "advice")};
    }
    return f;
}

int TodoMinutes(const Item& item) {
    const std::string& t = item.time.empty() ? item.title : item.time;
    int h = 0, m = 0, used = 0;
    if (std::sscanf(t.c_str(), "%2d:%2d%n", &h, &m, &used) == 2 && used >= 4 && h >= 0 && h < 24 &&
        m >= 0 && m < 60 && (item.time.size() || (t.size() > size_t(used) && t[used] == ' ')))
        return h * 60 + m;
    return 24 * 60;
}

std::string TodoLabel(const Item& item) {
    if (TodoMinutes(item) >= 24 * 60)
        return item.title;
    const auto space = item.title.find(' ');
    return space == std::string::npos || space > 5 ? item.title : item.title.substr(space + 1);
}

namespace {
// Plain function comparators: both sorts share one std::sort instantiation.
bool NewerFirst(const Item* a, const Item* b) { return a->ts != b->ts ? a->ts > b->ts : a->rev > b->rev; }
bool EarlierTodo(const Item* a, const Item* b) {
    const int ma = TodoMinutes(*a), mb = TodoMinutes(*b);
    return ma != mb ? ma < mb : a->rev < b->rev;
}
}  // namespace

const Item* Data::TodayFortune(const std::string& today) const {
    const Item* best = nullptr;
    for (const auto& item : items)
        if (item.kind() == Kind::Fortune && item.Day() == today && (!best || item.rev > best->rev))
            best = &item;
    return best;
}

std::vector<const Item*> Data::TodayStudy(const std::string& today) const {
    std::vector<const Item*> out;
    for (const auto& item : items) {
        const Kind kind = item.kind();
        if ((kind == Kind::Paper || kind == Kind::Word || kind == Kind::Point) && item.Day() == today)
            out.push_back(&item);
    }
    std::sort(out.begin(), out.end(), NewerFirst);
    return out;
}

std::vector<const Item*> Data::TodayTodos(const std::string& today) const {
    std::vector<const Item*> out;
    for (const auto& item : items)
        if (item.kind() == Kind::Todo && item.Day() == today)
            out.push_back(&item);
    std::sort(out.begin(), out.end(), EarlierTodo);
    return out;
}

std::vector<DaySummary> Data::Review(const std::string& today) const {
    std::vector<DaySummary> out;
    std::vector<int> fortune_rev;
    for (const auto& item : items) {
        const std::string day = item.Day();
        if (day.empty() || (!today.empty() && day >= today))
            continue;
        // Kept newest day first as it is built.
        size_t at = 0;
        while (at < out.size() && out[at].day > day)
            ++at;
        if (at == out.size() || out[at].day != day) {
            out.insert(out.begin() + at, DaySummary{});
            out[at].day = day;
            out[at].fav = marks.fav_days.count(day);
            fortune_rev.insert(fortune_rev.begin() + at, -1);
        }
        DaySummary& d = out[at];
        switch (item.kind()) {
        case Kind::Fortune:
            if (item.rev >= fortune_rev[at]) {
                fortune_rev[at] = item.rev;
                const Fortune f = ParseFortune(item.body);
                d.score = f.score;
                d.fortune = f.summary.empty() ? item.title : f.summary;
            }
            break;
        case Kind::Todo:
            ++d.todo_total;
            if (marks.done.count(item.id))
                ++d.done;
            else
                d.undone.push_back(TodoLabel(item));
            break;
        case Kind::Paper:
        case Kind::Word:
        case Kind::Point:
            ++d.study_total;
            d.learned += marks.learned.count(item.id) ? 1 : 0;
            d.study.push_back(item.title);
            break;
        case Kind::Other:
            break;
        }
    }
    return out;
}

int Data::Unread(const std::string& today) const {
    int count = 0;
    for (const auto& item : items)
        count += item.Day() == today && item.kind() != Kind::Other && !marks.read.count(item.id);
    return count;
}

Store& Store::GetInstance() {
    static Store store;
    return store;
}

#ifndef ESP_PLATFORM
void Store::ResetForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_ = true;
    persist_ = false;
    since_ = 0;
    items_.clear();
    marks_ = {};
    ever_synced_ = online_ = false;
    PublishLocked();
}
#endif

void Store::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_)
        return;
    loaded_ = true;
#ifdef ESP_PLATFORM
    if (Tab5SdReady()) {
        FILE* file = std::fopen(kFile, "rb");
        if (file) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            std::string text(size > 0 && size <= kMaxFileBytes ? size_t(size) : 0, '\0');
            const size_t got = std::fread(text.data(), 1, text.size(), file);
            std::fclose(file);
            Json root(cJSON_ParseWithLength(text.data(), got), cJSON_Delete);
            if (cJSON_IsObject(root.get())) {
                since_ = Int(root.get(), "since");
                const cJSON* object = nullptr;
                cJSON_ArrayForEach(object, cJSON_GetObjectItemCaseSensitive(root.get(), "items")) {
                    Item item;
                    if (ParseItem(object, &item))
                        items_.push_back(std::move(item));
                }
                for (int i = 0; i < 5; ++i)
                    for (auto& key : Strings(root.get(), kMarkKeys[i], 4000))
                        MarkSet(&marks_, Mark(i))->insert(std::move(key));
                ever_synced_ = !items_.empty();
            }
            ESP_LOGI(TAG, "history: %u item(s), since=%d", unsigned(items_.size()), since_);
        }
    }
#endif
    PublishLocked();
}

bool Store::ApplyInbox(const std::string& body, bool* more) {
    *more = false;
    Json root(cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    const auto* list = cJSON_GetObjectItemCaseSensitive(root.get(), "items");
    if (!cJSON_IsObject(root.get()) || !cJSON_IsArray(list))
        return false;
    const int rev = Int(root.get(), "rev", -1);
    const int latest = Int(root.get(), "latest_rev", -1);
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = !ever_synced_;
    ever_synced_ = true;
    if (latest >= 0 && latest < since_) {
        // The relay started over with a fresh data folder: pull everything again.
        since_ = 0;
        *more = true;
        return changed;
    }
    const cJSON* object = nullptr;
    cJSON_ArrayForEach(object, list) {
        const std::string id = Str(object, "id");
        const auto at = std::find_if(items_.begin(), items_.end(),
                                     [&](const Item& item) { return item.id == id; });
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "deleted"))) {
            if (at != items_.end()) {
                items_.erase(at);
                changed = true;
            }
            continue;
        }
        Item item;
        if (!ParseItem(object, &item))
            continue;
        if (at != items_.end())
            items_.erase(at);
        items_.push_back(std::move(item));
        changed = true;
    }
    if (items_.size() > kMaxItems)
        items_.erase(items_.begin(), items_.begin() + (items_.size() - kMaxItems));
    if (rev > since_) {
        since_ = rev;
        changed = true;
    }
    *more = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "more"));
    if (changed) {
        SaveLocked();
        PublishLocked();
    }
    return changed;
}

void Store::SetOnline(bool online) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (online_ == online)
        return;
    online_ = online;
    PublishLocked();
}

int Store::Since() {
    std::lock_guard<std::mutex> lock(mutex_);
    return since_;
}

SharedData Store::Get() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shared_)
        PublishLocked();
    return shared_;
}

void Store::SetMark(Mark mark, const std::string& key, bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* set = MarkSet(&marks_, mark);
    if (!(value ? set->insert(key) : set->erase(key)))
        return;
    SaveLocked();
    PublishLocked();
}

void Store::MarkAllRead(const std::string& today) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (const auto& item : items_)
        if (item.Day() == today)
            changed |= marks_.read.insert(item.id);
    if (!changed)
        return;
    SaveLocked();
    PublishLocked();
}

void Store::Prune(const std::string& today) {
    if (today.empty())
        return;
    const std::string first = ShiftDay(today, -kRetainDays);
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const Item& item) { return item.Day() < first; }),
                 items_.end());
    KeySet ids;
    for (const auto& item : items_)
        ids.insert(item.id);
    size_t marks = 0;
    for (int i = 0; i < 4; ++i)
        marks += MarkSet(&marks_, Mark(i))->KeepIf([&](const std::string& key) { return ids.count(key); });
    marks += marks_.fav_days.KeepIf([&](const std::string& day) { return day >= first; });
    if (items_.size() == before && marks == 0)
        return;
    SaveLocked();
    PublishLocked();
}

void Store::PublishLocked() {
    auto data = std::make_shared<Data>();
    data->items = items_;
    data->marks = marks_;
    data->version = ++version_;
    data->ever_synced = ever_synced_;
    data->online = online_;
    shared_ = std::move(data);
}

void Store::SaveLocked() {
#ifdef ESP_PLATFORM
    if (!persist_ || !Tab5SdReady())
        return;
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddNumberToObject(root.get(), "v", 1);
    cJSON_AddNumberToObject(root.get(), "since", since_);
    cJSON* list = cJSON_AddArrayToObject(root.get(), "items");
    for (const auto& item : items_)
        cJSON_AddItemToArray(list, ItemJson(item));
    for (int i = 0; i < 5; ++i) {
        cJSON* keys = cJSON_AddArrayToObject(root.get(), kMarkKeys[i]);
        for (const auto& key : MarkSet(&marks_, Mark(i))->keys())
            cJSON_AddItemToArray(keys, cJSON_CreateString(key.c_str()));
    }
    char* text = cJSON_PrintUnformatted(root.get());
    if (!text)
        return;
    mkdir("/sdcard/tab5", 0775);
    mkdir(kDir, 0775);
    const std::string temporary = std::string(kFile) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    const size_t length = std::strlen(text);
    bool ok = file && std::fwrite(text, 1, length, file) == length;
    if (file)
        ok = std::fflush(file) == 0 && fsync(fileno(file)) == 0 && std::fclose(file) == 0 && ok;
    cJSON_free(text);
    if (ok) {
        unlink(kFile);
        ok = rename(temporary.c_str(), kFile) == 0;
    }
    if (!ok)
        ESP_LOGW(TAG, "history save failed");
#else
    (void)persist_;
#endif
}

std::string Today() {
    const time_t now = time(nullptr);
    if (now < 1704067200)  // 2024-01-01: the clock has not been set yet
        return {};
    struct tm local {};
    localtime_r(&now, &local);
    char text[16];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
    return text;
}

// Proleptic Gregorian day arithmetic (H. Hinnant's days_from_civil / civil_from_days);
// newlib has no timegm.
std::string ShiftDay(const std::string& day, int days) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(day.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31)
        return {};
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long z = long(era) * 146097 + doe - 719468 + days + 719468;
    const long era2 = (z >= 0 ? z : z - 146096) / 146097;
    const long doe2 = z - era2 * 146097;
    const long yoe2 = (doe2 - doe2 / 1460 + doe2 / 36524 - doe2 / 146096) / 365;
    const long doy2 = doe2 - (365 * yoe2 + yoe2 / 4 - yoe2 / 100);
    const long mp = (5 * doy2 + 2) / 153;
    const int d2 = int(doy2 - (153 * mp + 2) / 5 + 1);
    const int m2 = int(mp < 10 ? mp + 3 : mp - 9);
    const long y2 = yoe2 + era2 * 400 + (m2 <= 2);
    char text[40];
    std::snprintf(text, sizeof(text), "%04ld-%02d-%02d", y2, m2, d2);
    return text;
}

}  // namespace tab5_nabo
