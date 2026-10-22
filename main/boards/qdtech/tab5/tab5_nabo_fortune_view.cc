// 今日运势 board of the 每日推送 page: nine tabs (综合 plus eight module cards) over the
// pushed nabo/fortune JSON body. A missing or empty module shows 「暂无」.
#include <algorithm>
#include <cstdint>
#include <string>

#include "tab5_nabo_push_page.h"
#include "tab5_nabo_ui.h"

LV_FONT_DECLARE(qd_font_lyric_48);

using namespace tab5_nabo;
using namespace tab5_nabo_ui;

namespace {

constexpr const char* kModules[] = {"综合", "称骨", "周易", "灵签", "塔罗",
                                    "黄历", "星座", "紫微", "奇门"};
constexpr uint32_t kBarColors[] = {kBlue, kGreen, kPink, 0xE8C27A, kOrange};

// The 48 px WenKai face holds a common-character subset; use it only when every glyph is in it.
const lv_font_t* BigOr(const std::string& text, const lv_font_t* fallback) {
    lv_font_glyph_dsc_t glyph;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        uint32_t code = n == 1 ? c : c & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < text.size(); ++k)
            code = (code << 6) | (text[i + k] & 0x3F);
        i += n;
        if (code > ' ' && !lv_font_get_glyph_dsc(&qd_font_lyric_48, &glyph, code, 0))
            return fallback;
    }
    return &qd_font_lyric_48;
}

std::string OrNone(const std::string& text) { return text.empty() ? "暂无" : text; }

size_t CodePoints(const std::string& text) {
    size_t n = 0;
    for (unsigned char c : text)
        n += (c & 0xC0) != 0x80;
    return n;
}

}  // namespace

void Tab5NaboPushPage::RenderFortune(lv_obj_t* area) {
    for (int i = 0; i < 9; ++i) {
        auto* tab = Pill(area, kModules[i], module_ == i, 104, 48, OnModule, i);
        lv_obj_set_pos(tab, 12 + i * 112, 0);
    }
    const Item* item = data_->TodayFortune(today_);
    if (!item) {
        Txt(area, data_->ever_synced ? "今天的运势还没推送" : "还没连上中转站", kMuted, 12, 240, 1000);
        return;
    }
    const Fortune f = ParseFortune(item->body);
    if (module_ != 0) {
        auto* card = Panel(area, 12, 62, 1000, 546, kCard, 22);
        RenderFortuneModule(card, f);
        return;
    }

    auto* left = Column(area, 12, 62, 596, 546);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(left, Hex(kCard), 0);
    lv_obj_set_style_radius(left, 22, 0);
    lv_obj_set_style_pad_all(left, 28, 0);
    std::string meta = "来自 Nabo";
    if (item->ts.size() >= 16)
        meta += " · " + item->ts.substr(11, 5);
    Txt(left, meta, kMuted, 0, 0, 540);
    if (f.score) {
        auto* score = Row(left, 540);
        lv_obj_set_style_pad_column(score, 16, 0);
        ScoreDots(score, f.score, 26);
        Label(score, std::to_string(f.score) + " / 5", &lv_font_montserrat_20, kOrange, 0, 0, LV_SIZE_CONTENT);
    } else {
        Txt(left, "评分 暂无", kOrange, 0, 0, 540);
    }
    const std::string summary = OrNone(f.summary);
    Txt(left, summary, kText, 0, 0, 540, BigOr(summary, Text()));
    Chips(left, "宜", kGreen, f.yi, 540);
    Chips(left, "忌", kRose, f.ji, 540);
    auto* note = Panel(left, 0, 0, 540, LV_SIZE_CONTENT, kNote, 16);
    lv_obj_set_style_pad_all(note, 16, 0);
    Txt(note, "Nabo：" + OrNone(f.nabo), kNoteText, 0, 0, 508);

    auto* color = Panel(area, 628, 62, 184, 120, kCard, 22);
    Txt(color, "幸运色", kMuted, 22, 14, 150);
    Txt(color, OrNone(f.color), kText, 22, 60, 150);
    auto* number = Panel(area, 828, 62, 184, 120, kCard, 22);
    Txt(number, "幸运数字", kMuted, 22, 14, 150);
    Label(number, f.number.empty() ? "—" : f.number, &lv_font_montserrat_48, kOrange, 22, 56, 150);
    auto* bars = Panel(area, 628, 198, 384, 410, kCard, 22);
    if (f.bars.empty())
        Txt(bars, "分项 暂无", kMuted, 24, 24, 336);
    for (size_t i = 0; i < f.bars.size() && i < 5; ++i) {
        const int y = 30 + int(i) * 76;
        Txt(bars, f.bars[i].first, kText, 24, y, 130);
        auto* track = Panel(bars, 160, y + 16, 168, 12, kChip, 6);
        Panel(track, 0, 0, 168 * std::max(0, std::min(5, f.bars[i].second)) / 5, 12, kBarColors[i], 6);
        Label(bars, std::to_string(f.bars[i].second), &lv_font_montserrat_20, kSoft, 342, y + 8, 30);
    }
}

void Tab5NaboPushPage::RenderFortuneModule(lv_obj_t* card, const Fortune& f) {
    Txt(card, kModules[module_], kText, 34, 24, 200);
    Txt(card, "来自 Nabo", kMuted, 120, 24, 300);
    auto* body = Column(card, 34, 80, 932, 446);
    const auto big = [&](const std::string& text, uint32_t color) {
        return Txt(body, text, color, 0, 0, 916, BigOr(text, Text()));
    };
    const auto note = [&](const std::string& text) {
        if (text.empty())
            return;
        auto* box = Panel(body, 0, 0, 916, LV_SIZE_CONTENT, kNote, 16);
        lv_obj_set_style_pad_all(box, 16, 0);
        Txt(box, text, kNoteText, 0, 0, 884);
    };
    const auto tile = [&](lv_obj_t* row, const std::string& label, const std::string& value, uint32_t tint,
                          int w) {
        auto* box = Panel(row, 0, 0, w, LV_SIZE_CONTENT, tint, 16);
        lv_obj_set_style_pad_all(box, 18, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        Txt(box, label, kMuted, 0, 0, w - 36);
        // Four 48 px characters do not fit a quarter-width tile.
        Txt(box, OrNone(value), kText, 0, 0, w - 36,
            CodePoints(value) * 48 <= size_t(w - 36) ? BigOr(value, Text()) : Text());
    };
    bool present = false;
    switch (module_) {
    case 1:
        if ((present = f.chenggu.present)) {
            big(OrNone(f.chenggu.weight), kOrange);
            for (const auto& line : f.chenggu.verse)
                big(line, kText);
            note(f.chenggu.plain.empty() ? "" : "白话：" + f.chenggu.plain);
        }
        break;
    case 2:
        if ((present = f.zhouyi.present)) {
            auto* row = Row(body, 916);
            lv_obj_set_style_pad_column(row, 40, 0);
            for (int which = 0; which < 2; ++which) {
                const int* lines = which ? f.zhouyi.bian_lines : f.zhouyi.gua_lines;
                auto* lines_box = Panel(row, 0, 0, 120, 6 * 21, kBg, 0);
                for (int k = 0; k < 6; ++k) {  // drawn top line first
                    const int line = 6 - k;
                    bool moving = false;
                    for (int m : f.zhouyi.moving)
                        moving |= !which && m == line;
                    const uint32_t color = moving ? kOrange : kBody;
                    if (lines[line - 1])
                        Panel(lines_box, 0, k * 21, 120, 12, color, 3);
                    else {
                        Panel(lines_box, 0, k * 21, 52, 12, color, 3);
                        Panel(lines_box, 68, k * 21, 52, 12, color, 3);
                    }
                }
                const std::string& name = which ? f.zhouyi.bian : f.zhouyi.gua;
                auto* label = Panel(row, 0, 0, 260, LV_SIZE_CONTENT, kBg, 0);
                lv_obj_set_flex_flow(label, LV_FLEX_FLOW_COLUMN);
                Txt(label, which ? "变卦" : "本卦", kMuted, 0, 0, 240);
                Txt(label, OrNone(name), kText, 0, 0, 240, BigOr(name, Text()));
            }
            if (!f.zhouyi.moving.empty()) {
                std::string moving = "动爻：第";
                for (int m : f.zhouyi.moving)
                    moving += " " + std::to_string(m);
                Txt(body, moving + " 爻，橙色标出", kMuted, 0, 0, 916);
            }
            note(f.zhouyi.plain.empty() ? "" : "卦辞白话：" + f.zhouyi.plain);
        }
        break;
    case 3:
        if ((present = f.lingqian.present)) {
            big(OrNone(f.lingqian.no) + (f.lingqian.level.empty() ? "" : " · " + f.lingqian.level), kOrange);
            for (const auto& line : f.lingqian.poem)
                big(line, kText);
            note(f.lingqian.explain.empty() ? "" : "解签：" + f.lingqian.explain);
        }
        break;
    case 4:
        if ((present = !f.tarot.empty())) {
            auto* row = Row(body, 916);
            lv_obj_set_style_pad_column(row, 16, 0);
            for (const auto& c : f.tarot) {
                auto* box = Panel(row, 0, 0, 294, 330, 0x1F2530, 18);
                Txt(box, OrNone(c.pos), kMuted, 20, 18, 254);
                Txt(box, OrNone(c.name), kText, 20, 60, 254);
                Tag(box, c.upright ? "正位" : "逆位", c.upright ? kGreen : kRose, 20, 112, Text());
                Txt(box, c.meaning, kBody, 20, 168, 254);
            }
        }
        break;
    case 5:
        if ((present = f.huangli.present)) {
            auto* row = Row(body, 916);
            const std::string day = f.huangli.ganzhi.size() > 6
                                        ? f.huangli.ganzhi.substr(f.huangli.ganzhi.rfind(' ') + 1)
                                        : f.huangli.ganzhi;
            tile(row, "农历", f.huangli.lunar, 0x1F2530, 220);
            tile(row, "日干支", day, 0x1F2530, 220);
            tile(row, "值神", f.huangli.zhishen, 0x1F2530, 220);
            tile(row, "建除", f.huangli.jianchu, 0x1F2530, 220);
            if (!f.huangli.ganzhi.empty())
                Txt(body, f.huangli.ganzhi, kMuted, 0, 0, 916);
            Chips(body, "宜", kGreen, f.huangli.yi, 916);
            Chips(body, "忌", kRose, f.huangli.ji, 916);
        }
        break;
    case 6:
        if ((present = f.xingzuo.present)) {
            big(OrNone(f.xingzuo.sign), kText);
            auto* row = Row(body, 916);
            for (const auto& entry : f.xingzuo.index) {
                auto* box = Panel(row, 0, 0, 220, 120, 0x1F2530, 16);
                Txt(box, entry.first, kMuted, 18, 14, 184);
                Label(box, std::to_string(entry.second), &lv_font_montserrat_48, kOrange, 18, 54, 184);
            }
            note(f.xingzuo.line);
        }
        break;
    case 7:
        if ((present = f.ziwei.present)) {
            auto* row = Row(body, 916);
            tile(row, "命宫主星", f.ziwei.main, 0x1F2530, 450);
            tile(row, "流日宫位", f.ziwei.palace, 0x1F2530, 450);
            note(f.ziwei.comment.empty() ? "" : "简评：" + f.ziwei.comment);
        }
        break;
    case 8:
        if ((present = f.qimen.present)) {
            auto* row = Row(body, 916);
            tile(row, "吉方位", f.qimen.good, 0x18302A, 450);
            tile(row, "凶方位", f.qimen.bad, 0x35202A, 450);
            note(f.qimen.advice.empty() ? "" : "建议：" + f.qimen.advice);
        }
        break;
    }
    if (!present) {
        Txt(body, "暂无", kSoft, 0, 0, 916);
        Txt(body, "今天的推送里没有这一项", kMuted, 0, 0, 916);
    }
}
