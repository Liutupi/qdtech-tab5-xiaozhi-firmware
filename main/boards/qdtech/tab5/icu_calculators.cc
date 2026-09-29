#include "icu_calculators.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

namespace icu {
namespace {

constexpr DrugInfo kDrugs[] = {
    {"去甲肾上腺素", "mg", "μg/kg/min", true, "核对药物盐型与标签含量"},
    {"肾上腺素", "mg", "μg/kg/min", true, "核对药物标签含量"},
    {"阿拉明", "mg", "μg/kg/min", true, "阿拉明即间羟胺；核对制剂含量"},
    {"多巴胺", "mg", "μg/kg/min", true, "核对药物标签含量"},
    {"多巴酚丁胺", "mg", "μg/kg/min", true, "核对药物标签含量"},
    {"胺碘酮", "mg", "mg/min", false, "稀释液按产品说明核对；常用制剂要求葡萄糖液"},
    {"奥美拉唑", "mg", "mg/h", false, "核对制剂说明书与配伍"},
    {"血管加压素", "U", "U/min", false, "输入总单位 U，不是 mg"},
    {"生长抑素", "mg", "μg/h", false, "输入药物总量 mg"},
    {"奥曲肽", "mg", "μg/h", false, "输入药物总量 mg"},
    {"胰岛素", "U", "U/h", false, "输入总单位 U，不是 mg"},
    {"呋塞米", "mg", "mg/h", false, "核对药物标签含量"},
    {"右美托咪定", "mg", "μg/kg/h", true, "输入总量 mg；核对制剂含量"},
};

bool Finite(double x) { return std::isfinite(x); }
Result Invalid(const char* reason) { return {false, reason}; }
}  // namespace

const DrugInfo& GetDrugInfo(Drug drug) {
    const int index = static_cast<int>(drug);
    return kDrugs[(index >= 0 && index < kDrugCount) ? index : 0];
}

bool ParseDecimal(const std::string& input, double& value) {
    if (input.empty()) return false;
    char* end = nullptr;
    errno = 0;
    value = std::strtod(input.c_str(), &end);
    return errno == 0 && end != input.c_str() && *end == '\0' && Finite(value);
}

Result Egfr(double age, bool female, double scr_umol_l) {
    if (!Finite(age) || age < 18 || age > 120) return Invalid("年龄限 18–120 岁；成人 CKD-EPI 公式不适用于儿童。");
    if (!Finite(scr_umol_l) || scr_umol_l <= 0 || scr_umol_l > 3000)
        return Invalid("请输入有效的血肌酐，单位 μmol/L。");
    const double scr_mg_dl = scr_umol_l / 88.4;
    const double kappa = female ? 0.7 : 0.9;
    const double alpha = female ? -0.241 : -0.302;
    const double ratio = scr_mg_dl / kappa;
    const double egfr = 142 * std::pow(std::fmin(ratio, 1.0), alpha)
        * std::pow(std::fmax(ratio, 1.0), -1.200) * std::pow(0.9938, age)
        * (female ? 1.012 : 1.0);
    const char* category = egfr >= 90 ? "G1" : egfr >= 60 ? "G2" :
        egfr >= 45 ? "G3a" : egfr >= 30 ? "G3b" : egfr >= 15 ? "G4" : "G5";
    char buffer[420];
    std::snprintf(buffer, sizeof(buffer),
        "eGFR  %.1f mL/min/1.73m²\nKDIGO 滤过率分层：%s\n\n输入：年龄 %.0f 岁，%s，血肌酐 %.1f μmol/L。\n2021 CKD-EPI 肌酐公式。\n急性肾损伤、肌酐未稳定时估算可能失准；单次数值不能诊断 CKD。",
        egfr, category, age, female ? "女" : "男", scr_umol_l);
    return {true, buffer};
}

Result Uacr(double albumin_mg_l, double creatinine_mmol_l) {
    if (!Finite(albumin_mg_l) || albumin_mg_l < 0 || albumin_mg_l > 100000 ||
        !Finite(creatinine_mmol_l) || creatinine_mmol_l <= 0 || creatinine_mmol_l > 1000)
        return Invalid("请核对尿白蛋白 mg/L 和尿肌酐 mmol/L；尿肌酐必须大于 0。");
    const double mg_mmol = albumin_mg_l / creatinine_mmol_l;
    // 1 mmol creatinine = 113.12 mg = 0.11312 g.
    const double mg_g = mg_mmol / 0.11312;
    // KDIGO's displayed mg/g and mg/mmol cutoffs are rounded equivalents.
    // Classify in the input unit so exactly 3 mg/mmol begins A2.
    const char* category = mg_mmol < 3 ? "A1" : mg_mmol <= 30 ? "A2" : "A3";
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
        "uACR  %.1f mg/g\n       %.2f mg/mmol\nKDIGO 白蛋白尿分层：%s\n\n同一次尿样：尿白蛋白 %.2f mg/L ÷ 尿肌酐 %.2f mmol/L。",
        mg_g, mg_mmol, category, albumin_mg_l, creatinine_mmol_l);
    return {true, buffer};
}

Result Oxygen(double fio2, double pao2, double map) {
    if (!Finite(fio2) || fio2 < 21 || fio2 > 100 ||
        !Finite(pao2) || pao2 <= 0 || pao2 > 800 ||
        !Finite(map) || map < 0 || map > 100)
        return Invalid("FiO₂ 输入 21–100%，PaO₂ 输入 mmHg；平均气道压可留空。");
    const double pf = pao2 / (fio2 / 100.0);
    char buffer[360];
    if (map > 0) {
        const double oi = fio2 * map / pao2;
        std::snprintf(buffer, sizeof(buffer),
            "PaO₂/FiO₂  %.1f mmHg\n正式 OI  %.2f\n\n输入：FiO₂ %.1f%%，PaO₂ %.1f mmHg，平均气道压 %.1f cmH₂O。\nOI = FiO₂(%%) × 平均气道压 ÷ PaO₂。\n仅凭比值不能诊断 ARDS。",
            pf, oi, fio2, pao2, map);
    } else {
        std::snprintf(buffer, sizeof(buffer),
            "PaO₂/FiO₂  %.1f mmHg\n\n输入：FiO₂ %.1f%%，PaO₂ %.1f mmHg。\nP/F = PaO₂ ÷ FiO₂ 小数。\n填写平均气道压后显示正式 OI。\n仅凭比值不能诊断 ARDS。", pf, fio2, pao2);
    }
    return {true, buffer};
}

Result BloodGas(double ph, double paco2, double hco3, double sodium, double chloride,
                double albumin) {
    if (!Finite(ph) || ph < 6.5 || ph > 7.9 || !Finite(paco2) || paco2 <= 0 || paco2 > 150 ||
        !Finite(hco3) || hco3 <= 0 || hco3 > 70 ||
        !Finite(sodium) || sodium < 0 || sodium > 200 ||
        !Finite(chloride) || chloride < 0 || chloride > 200 ||
        !Finite(albumin) || albumin < 0 || albumin > 80)
        return Invalid("请核对 pH、PaCO₂(mmHg)、HCO₃⁻(mmol/L) 及可选电解质。");
    if ((sodium == 0) != (chloride == 0))
        return Invalid("计算阴离子间隙需同时输入 Na⁺ 和 Cl⁻。");
    if (albumin > 0 && sodium == 0)
        return Invalid("白蛋白校正阴离子间隙需同时输入 Na⁺ 和 Cl⁻。");
    char buffer[700];
    const char* ph_state = ph < 7.35 ? "酸血症" : ph > 7.45 ? "碱血症" : "pH 在参考范围";
    std::snprintf(buffer, sizeof(buffer),
        "pH %.2f：%s\nPaCO₂ %.1f mmHg；HCO₃⁻ %.1f mmol/L", ph, ph_state, paco2, hco3);
    std::string output(buffer);
    const double calculated_ph = 6.1 + std::log10(hco3 / (0.03 * paco2));
    if (std::fabs(calculated_ph - ph) > 0.08)
        output += "\n输入值与 Henderson–Hasselbalch 关系不符，请核对标本及单位。";
    if (ph < 7.35) {
        if (hco3 < 22 && paco2 > 45) output += "\n代谢性与呼吸性酸中毒可能并存。";
        else if (hco3 < 22) output += "\n以代谢性酸中毒方向为主，核对呼吸代偿。";
        else if (paco2 > 45) output += "\n以呼吸性酸中毒方向为主，核对急慢性代偿。";
    } else if (ph > 7.45) {
        if (hco3 > 26 && paco2 < 35) output += "\n代谢性与呼吸性碱中毒可能并存。";
        else if (hco3 > 26) output += "\n以代谢性碱中毒方向为主，核对呼吸代偿。";
        else if (paco2 < 35) output += "\n以呼吸性碱中毒方向为主，核对急慢性代偿。";
    } else if (hco3 < 22 || hco3 > 26 || paco2 < 35 || paco2 > 45) {
        output += "\npH 正常不能排除已代偿或混合性紊乱。";
    }
    if (sodium > 0) {
        const double ag = sodium - chloride - hco3;
        std::snprintf(buffer, sizeof(buffer), "\nAG（不含 K⁺） %.1f mmol/L", ag);
        output += buffer;
        if (albumin > 0) {
            const double corrected = ag + 2.5 * (4.0 - albumin / 10.0);
            std::snprintf(buffer, sizeof(buffer), "；白蛋白校正 %.1f", corrected);
            output += buffer;
        }
    }
    if (hco3 < 22 && ph < 7.35) {
        const double expected = 1.5 * hco3 + 8;
        std::snprintf(buffer, sizeof(buffer),
            "\nWinter 预计 PaCO₂ %.1f–%.1f mmHg", expected - 2, expected + 2);
        output += buffer;
        if (paco2 > expected + 2) output += "；实测偏高，注意合并呼吸性酸中毒";
        else if (paco2 < expected - 2) output += "；实测偏低，注意合并呼吸性碱中毒";
    } else if (hco3 > 26 && ph > 7.45) {
        const double expected = 40 + 0.7 * (hco3 - 24);
        std::snprintf(buffer, sizeof(buffer),
            "\n代谢性碱中毒预计 PaCO₂ 约 %.1f mmHg（±5）", std::fmin(expected, 55.0));
        output += buffer;
    } else if (paco2 > 45 && ph < 7.35) {
        const double delta = (paco2 - 40) / 10;
        std::snprintf(buffer, sizeof(buffer),
            "\n呼吸性酸中毒预计 HCO₃⁻：急性约 %.1f，慢性约 %.1f mmol/L",
            24 + delta, 24 + 3.5 * delta);
        output += buffer;
    } else if (paco2 < 35 && ph > 7.45) {
        const double delta = (40 - paco2) / 10;
        std::snprintf(buffer, sizeof(buffer),
            "\n呼吸性碱中毒预计 HCO₃⁻：急性约 %.1f，慢性约 %.1f mmol/L",
            24 - 2 * delta, 24 - 4.5 * delta);
        output += buffer;
    }
    output += "\n\n需结合病史、氧疗和完整化验判断；本页不作诊断。";
    return {true, output};
}

Result Pump(Drug drug, double amount, double volume, double rate, double weight) {
    const int index = static_cast<int>(drug);
    if (index < 0 || index >= kDrugCount) return Invalid("药物选择无效。");
    const auto& info = kDrugs[index];
    if (!Finite(amount) || amount <= 0 || amount > 100000 ||
        !Finite(volume) || volume <= 0 || volume > 1000 ||
        !Finite(rate) || rate < 0 || rate > 1000)
        return Invalid("请输入大于 0 的药量和最终总液量，以及不小于 0 的泵速。");
    if (info.needs_weight && (!Finite(weight) || weight <= 0 || weight > 500))
        return Invalid("该药物需要输入有效体重 kg。");
    const double concentration = amount / volume;
    const double per_hour = concentration * rate;
    double standardized = 0;
    switch (drug) {
    case Drug::Norepinephrine: case Drug::Epinephrine: case Drug::Metaraminol:
    case Drug::Dopamine: case Drug::Dobutamine:
        standardized = per_hour * 1000 / (weight * 60); break;
    case Drug::Amiodarone: standardized = per_hour / 60; break;
    case Drug::Omeprazole: case Drug::Furosemide: case Drug::Insulin:
        standardized = per_hour; break;
    case Drug::Vasopressin: standardized = per_hour / 60; break;
    case Drug::Somatostatin: case Drug::Octreotide:
        standardized = per_hour * 1000; break;
    case Drug::Dexmedetomidine: standardized = per_hour * 1000 / weight; break;
    }
    char weight_text[64] = {};
    if (info.needs_weight)
        std::snprintf(weight_text, sizeof(weight_text), "，体重 %.4g kg", weight);
    char buffer[600];
    std::snprintf(buffer, sizeof(buffer),
        "%s  %.4g %s\n\n浓度 %.4g %s/mL\n泵速 %.4g mL/h\n实际输注 %.4g %s/h\n\n输入 %.4g %s，最终总液量 %.4g mL%s\n%s\n仅作单位换算；按医嘱及药品说明核对。",
        info.name, standardized, info.output_unit, concentration, info.input_unit,
        rate, per_hour, info.input_unit, amount, info.input_unit, volume,
        weight_text, info.note);
    return {true, buffer};
}

}  // namespace icu
