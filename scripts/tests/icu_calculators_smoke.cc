#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "icu_calculators.h"

static double LeadingValue(const icu::Result& result) {
    assert(result.ok);
    double value = 0;
    assert(std::sscanf(result.text.c_str(), "%*s %lf", &value) == 1);
    return value;
}

static void Near(double actual, double expected, double tolerance = 0.02) {
    assert(std::fabs(actual - expected) <= tolerance);
}

int main() {
    // Independent dimensional checks and formula reference cases.
    Near(LeadingValue(icu::Egfr(60, false, 88.4)), 86.2, 0.2);
    Near(LeadingValue(icu::Uacr(30, 10)), 26.52, 0.05);
    assert(icu::Uacr(30, 10).text.find("A2") != std::string::npos);
    Near(LeadingValue(icu::Oxygen(40, 80, 10)), 200, 0.1);
    Near(LeadingValue(icu::Pump(icu::Drug::Norepinephrine, 4, 50, 3, 80)), 0.05, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Vasopressin, 20, 50, 3)), 0.02, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Amiodarone, 150, 50, 10)), 0.5, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Epinephrine, 1, 50, 3, 80)), 0.0125, 0.0001);
    Near(LeadingValue(icu::Pump(icu::Drug::Metaraminol, 20, 50, 6, 60)), 0.6667, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Dopamine, 200, 50, 3, 80)), 2.5, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Omeprazole, 80, 50, 5)), 8, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Somatostatin, 3, 50, 2)), 120, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Octreotide, 0.5, 50, 5)), 50, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Insulin, 50, 50, 2)), 2, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Furosemide, 100, 50, 3)), 6, 0.001);
    Near(LeadingValue(icu::Pump(icu::Drug::Dexmedetomidine, 0.2, 50, 2, 80)), 0.1, 0.001);
    assert(icu::BloodGas(7.20, 30, 12, 140, 100, 30).text.find("AG（不含 K⁺） 28.0") != std::string::npos);
    assert(icu::BloodGas(7.20, 30, 12, 140, 100, 30).text.find("Winter 预计 PaCO₂ 24.0–28.0") != std::string::npos);
    assert(icu::BloodGas(7.20, 30, 24).text.find("Henderson") != std::string::npos);
    assert(!icu::Egfr(17, false, 88.4).ok);
    assert(!icu::Pump(icu::Drug::Norepinephrine, 4, 50, 3).ok);
    assert(!icu::Pump(icu::Drug::Vasopressin, 20, 0, 3).ok);
    assert(!icu::Oxygen(0.4, 80).ok);  // FiO2 must be entered as percent.
    double parsed = 0;
    assert(!icu::ParseDecimal("12mg", parsed));
    assert(!icu::ParseDecimal("nan", parsed));
}
