#include "tab5_lrc.h"

#include <cassert>
#include <string>

int main() {
    tab5_lrc::Document lyrics;
    assert(
        tab5_lrc::Parse("[ti:Song]\n[offset:-250]\n"
                        "[00:01.50][00:03.500]第一句\n"
                        "[00:05]第二句\n",
                        lyrics));
    assert(lyrics.timed && lyrics.lines.size() == 3);
    assert(lyrics.lines[0].at_ms == 1250);
    assert(lyrics.lines[1].at_ms == 3250);
    assert(lyrics.lines[2].at_ms == 4750);
    assert(tab5_lrc::ActiveLine(lyrics, 1249) == -1);
    assert(tab5_lrc::ActiveLine(lyrics, 1250) == 0);
    assert(tab5_lrc::ActiveLine(lyrics, 3250) == 1);
    assert(tab5_lrc::ActiveLine(lyrics, 6000) == 2);

    assert(tab5_lrc::Parse("第一句\n第二句\n", lyrics));
    assert(!lyrics.timed && lyrics.lines.size() == 2);
    assert(tab5_lrc::ActiveLine(lyrics, 30000) == 0);
    assert(!tab5_lrc::Parse("[ti:Song]\n", lyrics));
    assert(!tab5_lrc::Parse(std::string(16 * 1024 + 1, 'x'), lyrics));
    return 0;
}
