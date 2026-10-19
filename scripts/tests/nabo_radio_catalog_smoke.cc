// Host reader for test_nabo_radio_catalog.py: prints what the Tab5 would see.
//   smoke dump <catalog.bin>          one line per station, then regions and types
//   smoke find <catalog.bin> <needle> index of the best match (-1 when none)
//   smoke id <url>                    StableIdForUrl
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "nabo_radio_catalog.h"

using namespace nabo_radio_catalog;

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "id") == 0) {
        std::printf("%u\n", StableIdForUrl(argv[2]));
        return 0;
    }
    if (argc < 3)
        return 2;
    Catalog catalog(std::malloc, std::free);
    if (!catalog.Open(argv[2])) {
        std::printf("rejected\n");
        return 1;
    }
    if (std::strcmp(argv[1], "find") == 0 && argc == 4) {
        // A buffer far smaller than the pool exercises the refill path.
        char buf[kMaxStringsBytes];
        std::string needle = argv[3];
        for (auto& c : needle)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        std::printf("%d\n", catalog.Find(needle.c_str(), buf, sizeof(buf)));
        return 0;
    }
    if (std::strcmp(argv[1], "dump") != 0)
        return 2;
    std::printf("resident %zu\n", catalog.ResidentBytes());
    char buf[kMaxStringsBytes];
    // Read back to front so every read seeks.
    for (int i = catalog.Count() - 1; i >= 0; --i) {
        Strings s;
        if (!catalog.ReadStrings(i, buf, sizeof(buf), &s))
            return 3;
        const Record r = catalog.At(i);
        std::printf("station %d %u %u %u %s|%s|%s|%s\n", i, r.id, r.heat, unsigned(r.types),
                    catalog.RegionName(r.region), s.name, s.url, s.py);
    }
    for (int i = 0; i < catalog.RegionCount(); ++i)
        std::printf("region %d %s %d\n", i, catalog.RegionName(i),
                    int(ClassifyRegion(catalog.RegionName(i))));
    for (int i = 0; i < catalog.TypeCount(); ++i)
        std::printf("type %d %s\n", i, catalog.TypeName(i));
    return 0;
}
