// Host harness: runs the real Tab5 native UI (LVGL 9.5) with virtual time and
// scripted events, writing 1280x720 RGB24 frames to stdout at 30 fps.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "tab5_native_display.h"
#include "cbin_font.h"
#include "board.h"
#include "freertos/queue.h"

static int64_t g_us = 0;
static time_t g_epoch_base = 0;
extern "C" int64_t host_time_us(void) { return g_us; }
extern "C" time_t time(time_t* t) {
    time_t v = g_epoch_base + g_us / 1000000;
    if (t) *t = v;
    return v;
}
static uint32_t TickCb() { return uint32_t(g_us / 1000); }

static std::map<std::string, std::string> g_settings;
int Settings::GetInt(const std::string& k, int d) {
    auto it = g_settings.find(ns_ + "." + k);
    return it == g_settings.end() ? d : atoi(it->second.c_str());
}
void Settings::SetInt(const std::string& k, int v) { g_settings[ns_ + "." + k] = std::to_string(v); }
std::string Settings::GetString(const std::string& k, const std::string& d) {
    auto it = g_settings.find(ns_ + "." + k);
    return it == g_settings.end() ? d : it->second;
}
void Settings::SetString(const std::string& k, const std::string& v) { g_settings[ns_ + "." + k] = v; }

static std::deque<std::function<void()>> g_tasks;
void Application::Schedule(std::function<void()> f) { g_tasks.push_back(std::move(f)); }
static bool g_playback = false;
bool AudioService::IsPlaybackIdle() const { return !g_playback; }
static bool g_waiting = false;
bool Application::IsWaitingForReply() const { return g_waiting; }
bool Tab5SdReady() { return true; }

static QdtechTab5Display* g_display = nullptr;
Board& Board::GetInstance() { static Board b; return b; }
Display* Board::GetDisplay() { return g_display; }

// tab5_muse::Inbox stub: the harness owns the snapshot.
namespace tab5_muse {
static Snapshot g_snap;
Inbox& Inbox::GetInstance() { static Inbox i; return i; }
void Inbox::RequestRefresh() {}
void Inbox::MarkAllSeen() {}
Snapshot Inbox::Current() { return g_snap; }
}  // namespace tab5_muse

// NES / gamepad stubs (game page is never opened).
#include "usb_gamepad_host.h"
uint8_t UsbGamepadNesMask() { return 0; }

static uint16_t g_fb[1280 * 720];
static void Flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
    const int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; ++y)
        std::memcpy(&g_fb[y * 1280 + a->x1], px + size_t(y - a->y1) * w * 2, size_t(w) * 2);
    lv_display_flush_ready(d);
}
static int g_tx = 0, g_ty = 0;
static bool g_pressed = false;
static void ReadTouch(lv_indev_t*, lv_indev_data_t* data) {
    if (getenv("TDEBUG") && g_pressed) fprintf(stderr, "read %d,%d t=%lld\n", g_tx, g_ty, (long long)g_us/1000);
    data->point.x = g_tx;
    data->point.y = g_ty;
    data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static std::vector<std::string> Split(const std::string& s, char c) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == c) { out.push_back(cur); cur.clear(); } else cur += ch;
    }
    out.push_back(cur);
    return out;
}
static std::string Unescape(std::string s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; ++i; } else o += s[i];
    }
    return o;
}
static std::string ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Podcast playback clock: starts when a podcast url is played.
static std::string g_pod_url;
static int64_t g_pod_start_us = -1;
static int g_level = 0;
static std::vector<int> g_levels;

static void LoadEpisodes(const std::string& path) {
    // Format: blocks separated by lines "==EP id|title|time|audio_url"; then "#script" lines,
    // "#track title|artist", "#cue start|end|kind|text".
    auto eps = tab5_podcast::MakeEpisodes();
    std::istringstream in(ReadFile(path));
    std::string line;
    tab5_podcast::Episode* cur = nullptr;
    while (std::getline(in, line)) {
        if (line.rfind("==EP ", 0) == 0) {
            auto p = Split(line.substr(5), '|');
            eps->emplace_back();
            cur = &eps->back();
            cur->id = atoi(p[0].c_str());
            cur->title = tab5_podcast::ToStr(p[1]);
            cur->time = tab5_podcast::ToStr(p[2]);
            cur->audio_url = tab5_podcast::ToStr(p.size() > 3 ? p[3] : "");
            cur->from = tab5_podcast::ToStr("Muse");
        } else if (cur && line.rfind("#script ", 0) == 0) {
            std::string s(cur->script.data(), cur->script.size());
            s += Unescape(line.substr(8)) + "\n";
            cur->script = tab5_podcast::ToStr(s);
        } else if (cur && line.rfind("#track ", 0) == 0) {
            auto p = Split(line.substr(7), '|');
            cur->tracks.push_back({tab5_podcast::ToStr(p[0]), tab5_podcast::ToStr(p[1])});
        } else if (cur && line.rfind("#cue ", 0) == 0) {
            auto p = Split(line.substr(5), '|');
            tab5_podcast::Cue c;
            c.start_ms = atoi(p[0].c_str());
            c.end_ms = atoi(p[1].c_str());
            c.music = p[2] == "music";
            c.text = tab5_podcast::ToStr(Unescape(p[3]));
            cur->cues.push_back(c);
        }
    }
    tab5_muse::g_snap.episodes = eps;
    tab5_muse::g_snap.podcast_ok = true;
    tab5_muse::g_snap.podcast_latest_id = eps->empty() ? 0 : eps->front().id;
}
static void LoadMessages(const std::string& path) {
    std::istringstream in(ReadFile(path));
    std::string line;
    tab5_muse::g_snap.messages.clear();
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto p = Split(line, '|');
        tab5_muse::Message m;
        m.id = atoi(p[0].c_str());
        m.title = p[1]; m.body = Unescape(p[2]); m.from = p[3]; m.time = p[4];
        tab5_muse::g_snap.messages.push_back(m);
    }
    tab5_muse::g_snap.ok = tab5_muse::g_snap.ever_ok = true;
    tab5_muse::g_snap.latest_id = tab5_muse::g_snap.messages.empty() ? 0 : tab5_muse::g_snap.messages.front().id;
    tab5_muse::g_snap.seen_id = 0;
    tab5_muse::g_snap.poll_count++;
}

struct Event { int64_t ms; std::string cmd, arg; };

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: harness script.txt duration_ms out.rgb [assets_dir]\n"); return 1; }
    const std::string script = argv[1];
    const int64_t duration_ms = atoll(argv[2]);
    FILE* out = std::fopen(argv[3], "wb");
    const std::string assets = argc > 4 ? argv[4] : ".";
    setenv("TZ", "CST-8", 1);
    tzset();
    struct tm base = {};
    base.tm_year = 2026 - 1900; base.tm_mon = 9; base.tm_mday = 6; base.tm_hour = 8; base.tm_min = 15; base.tm_sec = 0;
    g_epoch_base = mktime(&base);

    lv_init();
    lv_tick_set_cb(TickCb);
    lv_display_t* disp = lv_display_create(1280, 720);
    static std::vector<uint8_t> buf(1280 * 720 * 2);
    lv_display_set_buffers(disp, buf.data(), nullptr, buf.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, Flush);
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ReadTouch);

    // Theme text font = the Noto "common" 30 px font the device loads from SD.
    static std::string common = ReadFile(assets + "/font_noto_sans_common_30_4.bin");
    if (!common.empty()) {
        lv_font_t* f = cbin_font_create(reinterpret_cast<uint8_t*>(common.data()));
        LvglThemeManager::GetInstance().dark_.text_font_ = std::make_shared<LvglBuiltInFont>(f);
    }

    // Device SD card supplies cjk28.bin as fallback for the LXGW subsets (LoadSdFallbackFont).
    const_cast<lv_font_t*>(&qd_font_lxgw_28)->fallback = &qd_font_cjk_28;
    const_cast<lv_font_t*>(&qd_font_lxgw_36)->fallback = &qd_font_cjk_28;
    auto* display = new QdtechTab5Display(nullptr, nullptr, 1280, 720, 0, 0, false, false, false);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
    g_display = display;
    display->SetupUI();

    Tab5NativeApps::Actions actions;
    static const char* kStations[] = {"中国之声", "财经之声", "北京新闻广播", "北京交通广播", "北京音乐广播",
                                      "上海动感音乐", "广东新闻广播", "浙江音乐广播", "江苏新闻广播", "四川新闻广播"};
    static int station = 4;
    actions.wifi_summary = [] { return std::string("已连接：Tupi-Home  192.168.31.58"); };
    actions.get_brightness = [] { return 72; };
    actions.get_volume = [] { return 64; };
    actions.set_brightness = [](int) {};
    actions.set_volume = [](int) {};
    actions.station_count = [] { return 10; };
    actions.station_name = [](int i) { return std::string(kStations[i % 10]); };
    actions.radio_select = [](int i) { station = i; };
    actions.radio_level = [] { return g_level; };
    actions.radio_play_requested = [] { return g_playback; };
    actions.podcast_play_url = [](const std::string& url, const std::string&) {
        g_pod_url = url; g_pod_start_us = g_us; return true; };
    actions.podcast_play = [](int, int, int) { return true; };
    actions.podcast_position = [](std::string_view url) {
        tab5_playback::Position p;
        if (g_pod_start_us >= 0 && url == g_pod_url) {
            p.matches = true; p.generation = 1; p.milliseconds = uint32_t((g_us - g_pod_start_us) / 1000);
        }
        return p;
    };
    display->SetAppsActions(std::move(actions), [] {});

    // Load the event script.
    std::vector<Event> events;
    {
        std::istringstream in(ReadFile(script));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            Event e; ls >> e.ms >> e.cmd; std::getline(ls, e.arg);
            if (!e.arg.empty() && e.arg[0] == ' ') e.arg.erase(0, 1);
            events.push_back(e);
        }
    }
    size_t next_event = 0;
    std::vector<std::pair<int64_t, bool>> touches;  // release schedule
    const int64_t frame_us = 1000000 / 30;
    const int64_t step_us = 5000;
    int64_t next_frame = 0;
    int64_t warm = 0;  // optional pre-roll so the first frame is settled
    std::vector<uint8_t> rgb(1280 * 720 * 3);
    int64_t pre_roll_us = 0;
    for (auto& e : events) if (e.cmd == "preroll") pre_roll_us = std::llabs(e.ms) * 1000;
    for (g_us = -pre_roll_us; g_us <= duration_ms * 1000; g_us += step_us) {
        const int64_t now_ms = g_us / 1000;
        while (next_event < events.size() && events[next_event].ms <= now_ms) {
            const Event& e = events[next_event++];
            auto a = Split(e.arg, '|');
            if (e.cmd == "status") display->SetStatus(e.arg.c_str());
            else if (e.cmd == "chat") display->SetChatMessage(a[0].c_str(), Unescape(a.size() > 1 ? a[1] : "").c_str());
            else if (e.cmd == "emotion") display->SetEmotion(e.arg.c_str());
            else if (e.cmd == "touch") { int hold = 90; sscanf(e.arg.c_str(), "%d %d %d", &g_tx, &g_ty, &hold); g_pressed = true;
                touches.push_back({g_us + 1000 * hold, false}); }
            else if (e.cmd == "podseek") g_pod_start_us = g_us - atoll(e.arg.c_str()) * 1000;
            else if (e.cmd == "waiting") g_waiting = atoi(e.arg.c_str()) != 0;
            else if (e.cmd == "playback") g_playback = atoi(e.arg.c_str()) != 0;
            else if (e.cmd == "level") g_level = atoi(e.arg.c_str());
            else if (e.cmd == "levelfile") { std::istringstream in(ReadFile(e.arg)); int v; while (in >> v) g_levels.push_back(v); }
            else if (e.cmd == "radio") display->PostRadioState(a[0].c_str(), a[1].c_str(), a.size() > 2 ? a[2].c_str() : "");
            else if (e.cmd == "music") {
                display->BeginMusicTrack(a[0].c_str(), a[1].c_str());
                display->SetMusicInfo(a[0].c_str(), a[1].c_str(), "正在连接音源…");
                if (a.size() > 2 && !a[2].empty()) display->SetMusicLyrics(ReadFile(a[2]).c_str(), a[0].c_str());
            }
            else if (e.cmd == "musicinfo") display->SetMusicInfo(a[0].c_str(), a[1].c_str(), a[2].c_str());
            else if (e.cmd == "sleep") display->SetSleeping(atoi(e.arg.c_str()) != 0);
            else if (e.cmd == "daily") {
                const char* t[3] = {a[0].c_str(), a[2].c_str(), a[4].c_str()};
                std::string b0 = Unescape(a[1]), b1 = Unescape(a[3]), b2 = Unescape(a[5]);
                const char* b[3] = {b0.c_str(), b1.c_str(), b2.c_str()};
                display->SetDailyCards(t, b);
            }
            else if (e.cmd == "episodes") LoadEpisodes(e.arg);
            else if (e.cmd == "messages") LoadMessages(e.arg);
            else if (e.cmd == "inbox") display->SetMuseInbox(tab5_muse::g_snap, atoi(e.arg.c_str()) != 0);
            else if (e.cmd == "vision") display->SetVisionMessage(a[0].c_str(), a[1].c_str());
            else if (e.cmd == "welcome") display->WelcomeBack();
            else if (e.cmd == "radio_page") display->ShowRadioPage();
            else if (e.cmd == "icu") display->ShowIcuPage(atoi(e.arg.c_str()));
            else if (e.cmd == "idle") display->ShowIdlePanel();
            else if (e.cmd == "icu_result") { auto p = Split(e.arg, '|'); display->ShowIcuPage(atoi(p[0].c_str()), Unescape(p[1]), true); }
            else if (e.cmd == "fw") { auto p = Split(e.arg, '|'); display->SetFirmwareStatus(p[0], p[1], atoi(p[2].c_str()), p[3] == "1"); }
            else if (e.cmd == "dailypage") display->ShowDailyPage(atoi(e.arg.c_str()));
            else if (e.cmd == "stopmusic") display->StopMusicTrack();
            else if (e.cmd == "touchnabo") display->TouchNabo();
            else if (e.cmd == "page") {
                auto* ap = display->apps_.get();
                if (e.arg == "apps") ap->OpenApps();
                else if (e.arg == "radio") ap->OpenRadio(false);
                else if (e.arg == "podcast") ap->OpenPodcast();
                else if (e.arg == "ir") ap->OpenIr();
                else if (e.arg == "muse") ap->OpenMuse();
                else if (e.arg == "settings") ap->OpenSettings();
                else if (e.arg == "close") ap->Close();
                else if (e.arg.rfind("icu", 0) == 0) ap->OpenIcu(atoi(e.arg.c_str() + 3));
            }
        }
        if (!g_levels.empty() && g_us >= 0) { size_t i = size_t(g_us / 10000); g_level = g_levels[std::min(i, g_levels.size() - 1)]; }
        for (auto& t : touches)
            if (!t.second && g_us >= t.first) { g_pressed = false; t.second = true; }
        while (!HostQueue().empty()) { auto* job = static_cast<Tab5IrRemotePage::Job*>(HostQueue().front()); HostQueue().pop_front(); job->run(); delete job; }
        while (!g_tasks.empty()) { auto f = std::move(g_tasks.front()); g_tasks.pop_front(); f(); }
        lv_timer_handler();
        usleep(3500);  // let the SD reader thread keep pace with virtual time
        if (g_us >= 0 && g_us >= next_frame) {
            lv_refr_now(disp);
            for (int i = 0; i < 1280 * 720; ++i) {
                const uint16_t c = g_fb[i];
                rgb[i * 3] = ((c >> 11) & 0x1f) * 255 / 31;
                rgb[i * 3 + 1] = ((c >> 5) & 0x3f) * 255 / 63;
                rgb[i * 3 + 2] = (c & 0x1f) * 255 / 31;
            }
            std::fwrite(rgb.data(), 1, rgb.size(), out);
            next_frame += frame_us;
        }
    }
    std::fclose(out);
    return 0;
}
