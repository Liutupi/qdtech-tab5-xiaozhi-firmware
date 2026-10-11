// Host stand-ins for the network services added in v1.0.10: Home Assistant hub,
// Open-Meteo weather and the MD launcher. Data is set by the harness script.
#include <functional>
#include <memory>
#include "tab5_home_hub.h"
#include "tab5_md_launcher.h"
#include "tab5_sky_weather.h"

namespace host {
tab5_home::Status home_status;
std::function<void()> home_changed;   // harness pushes the status into the page
tab5_sky::WeatherNow weather;
std::shared_ptr<const tab5_md::Catalog> md_catalog;
}

namespace tab5_home {
Hub& Hub::GetInstance() { static Hub h; return h; }
void Hub::Start(Listener) {}
void Hub::SetPageVisible(bool) {}
void Hub::RequestRefresh() {}
Status Hub::Current() { return host::home_status; }
static void Mutate(const std::string& id, const std::string& action, const std::string& value) {
    if (!host::home_status.catalog) return;
    auto next = std::make_shared<Catalog>(*host::home_status.catalog);
    for (auto& d : next->devices) {
        if (std::string(d.id.data(), d.id.size()) != id) continue;
        if (action == "toggle") d.state = tab5_podcast::ToStr(d.on() ? "off" : (d.kind() == Kind::kClimate ? "cool" : "on"));
        else if (action == "on") d.state = tab5_podcast::ToStr(d.kind() == Kind::kClimate ? "cool" : "on");
        else if (action == "off") d.state = tab5_podcast::ToStr("off");
        else if (action == "temperature") d.target = std::stof(value);
    }
    host::home_status.catalog = next;
    host::home_status.version++;
}
bool Hub::Control(const std::string& id, const std::string& action, const std::string& value, Done done) {
    Mutate(id, action, value);
    if (done) done(true, "已执行");
    if (host::home_changed) host::home_changed();
    return true;
}
bool Hub::RunScene(const std::string&, bool, Done done) {
    if (done) done(true, "场景已执行");
    if (host::home_changed) host::home_changed();
    return true;
}
}  // namespace tab5_home

namespace tab5_sky {
WeatherService& WeatherService::GetInstance() { static WeatherService w; return w; }
void WeatherService::Start() {}
WeatherNow WeatherService::Current() { return host::weather; }
bool WeatherService::Post(void (*fn)(void*), void* arg) { fn(arg); return true; }
}  // namespace tab5_sky

namespace tab5_md {
std::shared_ptr<const Catalog> LoadCatalog() { return host::md_catalog; }
std::string Launch(const Game&) { return "（演示）MD 游戏在 ota_0 程序中运行"; }
}  // namespace tab5_md
