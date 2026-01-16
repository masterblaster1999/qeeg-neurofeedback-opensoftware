#include "qeeg/nf_metric_eval.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

static void expect(bool ok, const std::string& msg) {
  if (!ok) {
    std::cerr << "TEST FAILED: " << msg << "\n";
    std::exit(1);
  }
}

int main() {
  using namespace qeeg;

  // One channel, two bands.
  OnlineBandpowerFrame fr;
  fr.channel_names = {"Cz"};
  fr.bands = {BandDefinition{"alpha", 8.0, 12.0}, BandDefinition{"beta", 13.0, 30.0}};

  // Band metric (raw)
  fr.log10_power = false;
  fr.powers = {{100.0}, {10.0}}; // [band][channel]

  NfMetricSpec spec;
  spec.type = NfMetricSpec::Type::Band;
  {
    const double v = nf_eval_metric_band_or_ratio(fr, spec, /*ch*/0, /*band*/0, /*num*/0, /*den*/0);
    expect(std::fabs(v - 100.0) < 1e-9, "band metric should return band value");
  }

  // Ratio metric (raw)
  spec.type = NfMetricSpec::Type::Ratio;
  {
    const double r = nf_eval_metric_band_or_ratio(fr, spec, /*ch*/0, /*band*/0, /*num*/0, /*den*/1);
    expect(std::fabs(r - 10.0) < 1e-6, "raw ratio alpha/beta should be ~10");
  }

  // Ratio metric (log10): when powers[][] are already log10-transformed, the metric should be
  // log10(alpha/beta) = log10(alpha) - log10(beta).
  fr.log10_power = true;
  fr.powers = {{2.0}, {1.0}}; // log10(100)=2, log10(10)=1
  {
    const double lr = nf_eval_metric_band_or_ratio(fr, spec, /*ch*/0, /*band*/0, /*num*/0, /*den*/1);
    expect(std::fabs(lr - 1.0) < 1e-9, "log10 ratio should be 1.0 for 100/10");
  }

  // Asymmetry metric: log10(Pa/Pb) using either raw powers or log10 powers.
  OnlineBandpowerFrame fr2;
  fr2.channel_names = {"F4", "F3"};
  fr2.bands = {BandDefinition{"alpha", 8.0, 12.0}};

  NfMetricSpec asym;
  asym.type = NfMetricSpec::Type::Asymmetry;
  asym.band = "alpha";
  asym.channel_a = "F4";
  asym.channel_b = "F3";

  // Raw powers: log10(100/25)=log10(4)
  fr2.log10_power = false;
  fr2.powers = {{100.0, 25.0}};
  {
    const double a = nf_eval_metric_asymmetry(fr2, asym, /*ch_a*/0, /*ch_b*/1, /*band*/0);
    expect(std::fabs(a - std::log10(4.0)) < 1e-9, "raw asymmetry should be log10(4)");
  }

  // Log10 powers: log10(100)-log10(25) = log10(4)
  fr2.log10_power = true;
  fr2.powers = {{2.0, std::log10(25.0)}};
  {
    const double a = nf_eval_metric_asymmetry(fr2, asym, /*ch_a*/0, /*ch_b*/1, /*band*/0);
    expect(std::fabs(a - std::log10(4.0)) < 1e-9, "log10 asymmetry should be log10(4)");
  }

  std::cerr << "test_nf_metric_eval OK\n";
  return 0;
}
