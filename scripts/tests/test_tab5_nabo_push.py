"""Run the NABO daily push store on the host: relay pages, fortune JSON and the local review."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"

SMOKE = r'''
#include "tab5_nabo_push.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace tab5_nabo;

static std::string Page(int rev, int latest, bool more, const std::string& items) {
    return "{\"ok\":true,\"rev\":" + std::to_string(rev) + ",\"latest_rev\":" + std::to_string(latest) +
           ",\"more\":" + (more ? "true" : "false") + ",\"items\":[" + items + "]}";
}

static const char* kFortune = R"({"id":"f22","ts":"2026-10-22T07:00:00+08:00","source":"nabo/fortune","priority":"normal","mood":"happy","title":"今日运势","rev":1,"body":"{\"score\":4,\"summary\":\"稳中有进\",\"yi\":[\"整理文献\",\"早睡\"],\"ji\":[\"熬夜\"],\"color\":\"雾蓝\",\"number\":7,\"bars\":{\"事业科研\":4,\"健康\":3,\"家庭\":4,\"财运\":3,\"心情\":5},\"nabo\":\"带伞\",\"modules\":{\"zhouyi\":{\"gua\":\"地天泰\",\"gua_lines\":[1,1,1,0,0,0],\"bian\":\"地泽临\",\"bian_lines\":[1,1,0,0,0,0],\"moving\":[3,9],\"plain\":\"小往大来\"},\"tarot\":[{\"pos\":\"过去\",\"name\":\"宝剑三\",\"upright\":true,\"meaning\":\"过去了\"},{\"pos\":\"未来\",\"name\":\"权杖王牌\",\"upright\":false,\"meaning\":\"先打磨\"}],\"ziwei\":{},\"qimen\":{\"good\":\"东南\",\"bad\":\"西北\",\"advice\":\"上午办\"}}}"})";

int main() {
    assert(ShiftDay("2026-10-22", -30) == "2026-09-22");
    assert(ShiftDay("2026-03-01", -1) == "2026-02-28");
    assert(ShiftDay("2024-03-01", -1) == "2024-02-29");
    assert(ShiftDay("2026-12-31", 1) == "2027-01-01");
    assert(ShiftDay("bad", 1).empty());

    auto& store = Store::GetInstance();
    store.ResetForTest();
    bool more = true;
    assert(!store.ApplyInbox("not json", &more) && !more);

    const std::string todos =
        R"({"id":"t1","ts":"2026-10-22T07:45:00+08:00","source":"nabo/todo","priority":"high","title":"15:00 组会","time":"15:00","rev":2},)"
        R"({"id":"t2","ts":"2026-10-22T07:46:00+08:00","source":"nabo/todo","title":"换备份硬盘","rev":3},)"
        R"({"id":"t3","ts":"2026-10-22T07:47:00+08:00","source":"nabo/todo","title":"9:30 交 PPT","rev":4},)"
        R"({"id":"t0","ts":"2026-10-21T08:00:00+08:00","source":"nabo/todo","title":"整理病例","rev":5},)"
        R"({"id":"s1","ts":"2026-10-22T08:12:00+08:00","source":"nabo/study/paper","title":"今日文献","body":"要点","rev":6},)"
        R"({"id":"s0","ts":"2026-10-21T08:12:00+08:00","source":"nabo/study/word","title":"旧单词","rev":7},)"
        R"({"id":"x","ts":"2026-10-22T08:00:00+08:00","source":"nabo/review","title":"不收","rev":8},)"
        R"({"id":"bad","source":"nabo/todo","title":"缺 ts","rev":9})";
    assert(store.ApplyInbox(Page(9, 9, false, std::string(kFortune) + "," + todos), &more));
    assert(store.Since() == 9 && !more);
    auto data = store.Get();
    assert(data->items.size() == 7);  // review and the item without ts are dropped
    const std::string today = "2026-10-22";
    const auto* fortune = data->TodayFortune(today);
    assert(fortune && fortune->id == "f22");
    const Fortune f = ParseFortune(fortune->body);
    assert(f.ok && f.score == 4 && f.summary == "稳中有进" && f.number == "7" && f.yi.size() == 2);
    assert(f.bars.size() == 5 && f.bars[0].first == "事业科研" && f.bars[4].second == 5);
    assert(f.zhouyi.present && f.zhouyi.gua_lines[2] == 1 && f.zhouyi.bian_lines[2] == 0);
    assert(f.zhouyi.moving.size() == 1 && f.zhouyi.moving[0] == 3);  // 9 is not a line
    assert(f.tarot.size() == 2 && f.tarot[0].upright && !f.tarot[1].upright);
    assert(!f.ziwei.present && !f.chenggu.present && f.qimen.present && f.qimen.good == "东南");
    assert(!ParseFortune("[1]").ok && !ParseFortune("").ok);

    const auto todo = data->TodayTodos(today);
    assert(todo.size() == 3 && todo[0]->id == "t3" && todo[1]->id == "t1" && todo[2]->id == "t2");
    assert(TodoMinutes(*todo[0]) == 570 && TodoLabel(*todo[0]) == "交 PPT");
    assert(TodoMinutes(*todo[2]) == 24 * 60 && TodoLabel(*todo[2]) == "换备份硬盘");
    assert(data->TodayStudy(today).size() == 1);
    assert(data->Unread(today) == 5);

    store.SetMark(Mark::Read, "f22", true);
    store.SetMark(Mark::Done, "t0", true);
    store.SetMark(Mark::Learned, "s0", true);
    store.SetMark(Mark::FavDay, "2026-10-21", true);
    const uint32_t version = store.Get()->version;
    store.SetMark(Mark::Read, "f22", true);  // unchanged: no new snapshot
    assert(store.Get()->version == version);
    data = store.Get();
    assert(data->Unread(today) == 4);
    const auto review = data->Review(today);
    assert(review.size() == 1 && review[0].day == "2026-10-21" && review[0].fav);
    assert(review[0].todo_total == 1 && review[0].done == 1 && review[0].undone.empty());
    assert(review[0].study_total == 1 && review[0].learned == 1 && review[0].score == 0);

    // Same id replaces; a tombstone removes; the next page continues from rev.
    assert(store.ApplyInbox(Page(11, 11, false,
        R"({"id":"t2","ts":"2026-10-22T07:46:00+08:00","source":"nabo/todo","title":"18:00 换硬盘","rev":10},)"
        R"({"id":"t1","deleted":true,"rev":11})"), &more));
    data = store.Get();
    assert(data->TodayTodos(today).size() == 2 && data->TodayTodos(today)[1]->title == "18:00 换硬盘");
    store.MarkAllRead(today);
    assert(store.Get()->Unread(today) == 0);

    // A relay that started over makes the next poll begin from zero.
    assert(store.ApplyInbox(Page(0, 0, false, ""), &more) == false);
    assert(more && store.Since() == 0);

    // History older than 30 days and marks for removed items are pruned.
    assert(store.ApplyInbox(Page(12, 12, false,
        R"({"id":"old","ts":"2026-09-01T08:00:00+08:00","source":"nabo/todo","title":"旧事","rev":12})"), &more));
    store.SetMark(Mark::Done, "old", true);
    store.SetMark(Mark::FavDay, "2026-09-01", true);
    store.Prune(today);
    data = store.Get();
    for (const auto& item : data->items) assert(item.id != "old");
    assert(!data->marks.done.count("old") && !data->marks.fav_days.count("2026-09-01"));
    assert(data->marks.fav_days.count("2026-10-21"));
    store.Prune("");  // clock not set: nothing happens
    std::puts("nabo push ok");
    return 0;
}
'''


@unittest.skipUnless(shutil.which("c++") and shutil.which("cc") and (CJSON / "cJSON.c").exists(),
                     "Host compilers and cJSON required")
class NaboPushStoreTests(unittest.TestCase):
    def test_store_review_and_fortune(self):
        with tempfile.TemporaryDirectory() as tmp:
            temp = Path(tmp)
            (temp / "smoke.cc").write_text(SMOKE)
            subprocess.run(["cc", "-c", str(CJSON / "cJSON.c"), "-o", str(temp / "cjson.o")], check=True)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                            "-I", str(CJSON), str(BOARD / "tab5_nabo_push.cc"), str(temp / "smoke.cc"),
                            str(temp / "cjson.o"), "-o", str(temp / "smoke")], check=True)
            result = subprocess.run([str(temp / "smoke")], check=True, capture_output=True, text=True)
            self.assertIn("nabo push ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
