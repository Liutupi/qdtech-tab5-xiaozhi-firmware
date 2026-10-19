// Host run of RadioCatalog for test_nabo_radio_catalog.py.
//   smoke <catalog.bin|none> <home region> [recent id...]
// Prints the source, each station (band, label, name, urls), the dial bands
// and a few searches.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "radio_catalog.h"

bool Tab5SdReady() { return true; }

static std::vector<RadioListEntry> Fallback() {
    std::vector<RadioListEntry> list(3);
    list[0].info.name = "中国之声";
    list[0].info.urls[0] = "https://lhttp.qtfm.cn/live/15318317/64k.mp3";
    list[0].region = "全国";
    list[0].types = RadioCatalog::ListTypeBit("新闻");
    list[1].info.name = "广东交通之声";
    list[1].info.urls[0] = "https://lhttp.qtfm.cn/live/1262/64k.mp3";
    list[1].region = "广东";
    list[1].types = RadioCatalog::ListTypeBit("交通");
    list[2].info.name = "Swiss Jazz";
    list[2].info.urls[0] = "https://stream.srg-ssr.ch/m/rsj/mp3_128";
    list[2].region = "海外";
    list[2].types = RadioCatalog::ListTypeBit("音乐");
    return list;
}

int main(int argc, char** argv) {
    if (argc < 3)
        return 2;
    RadioCatalog catalog;
    catalog.EnsureLoaded(Fallback, argv[1]);
    std::printf("source %s %d\n", catalog.FromCard() ? "card" : "list", catalog.Count());
    for (int i = 0; i < catalog.Count(); ++i) {
        RadioStationInfo info;
        if (!catalog.Load(i, &info))
            return 3;
        std::printf("station %d %u|%s|%s|%s|%s|%s|%s\n", i, catalog.StableId(i),
                    catalog.HomeBand(i), catalog.Label(i).c_str(), catalog.Name(i).c_str(),
                    info.urls[0].c_str(), info.urls[1].c_str(), info.urls[2].c_str());
    }
    std::vector<uint32_t> recent;
    for (int i = 3; i < argc; ++i)
        recent.push_back(uint32_t(std::strtoul(argv[i], nullptr, 10)));
    for (const auto& band : catalog.Bands(recent, argv[2])) {
        std::printf("band %s", band.name);
        for (uint16_t s : band.stations)
            std::printf(" %u", unsigned(s));
        std::printf("\n");
    }
    for (const char* needle : {"交通", "JAZZ", "xgdt", "不存在"}) {
        std::string name;
        std::printf("find %s %d %s\n", needle, catalog.Find(needle, &name), name.c_str());
    }
    return 0;
}
