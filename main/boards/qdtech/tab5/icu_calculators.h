#pragma once

#include <string>

namespace icu {

struct Result {
    bool ok;
    std::string text;
};

enum class Drug {
    Norepinephrine, Epinephrine, Metaraminol, Dopamine, Dobutamine,
    Amiodarone, Omeprazole, Vasopressin, Somatostatin, Octreotide,
    Insulin, Furosemide, Dexmedetomidine
};

struct DrugInfo {
    const char* name;
    const char* input_unit;
    const char* output_unit;
    bool needs_weight;
    const char* note;
};

constexpr int kDrugCount = 13;
const DrugInfo& GetDrugInfo(Drug drug);
Result Egfr(double age_years, bool female, double creatinine_umol_l);
Result Uacr(double urine_albumin_mg_l, double urine_creatinine_mmol_l);
Result Oxygen(double fio2_percent, double pao2_mmhg, double mean_airway_pressure_cmh2o = 0);
Result BloodGas(double ph, double paco2_mmhg, double hco3_mmol_l,
                double sodium_mmol_l = 0, double chloride_mmol_l = 0,
                double albumin_g_l = 0);
Result Pump(Drug drug, double drug_amount, double final_volume_ml,
            double rate_ml_h, double weight_kg = 0);
bool ParseDecimal(const std::string& input, double& value);

}  // namespace icu
