#include "qeeg/nf_metric.hpp"

#include "test_support.hpp"
#include <iostream>
#include <stdexcept>

using namespace qeeg;

int main() {
  {
    const auto m = parse_nf_metric_spec("alpha:Pz");
    assert(m.type == NfMetricSpec::Type::Band);
    assert(m.band == "alpha");
    assert(m.channel == "Pz");
  }

  {
    const auto m = parse_nf_metric_spec("alpha/beta:Pz");
    assert(m.type == NfMetricSpec::Type::Ratio);
    assert(m.band_num == "alpha");
    assert(m.band_den == "beta");
    assert(m.channel == "Pz");
  }

  {
    const auto m = parse_nf_metric_spec("asym:alpha:F4:F3");
    assert(m.type == NfMetricSpec::Type::Asymmetry);
    assert(m.band == "alpha");
    assert(m.channel_a == "F4");
    assert(m.channel_b == "F3");
  }

  {
    const auto m = parse_nf_metric_spec("asymmetry:alpha:F4:F3");
    assert(m.type == NfMetricSpec::Type::Asymmetry);
    assert(m.band == "alpha");
    assert(m.channel_a == "F4");
    assert(m.channel_b == "F3");
  }

  {
    const auto m = parse_nf_metric_spec("coh:alpha:F3:F4");
    assert(m.type == NfMetricSpec::Type::Coherence);
    assert(m.band == "alpha");
    assert(m.channel_a == "F3");
    assert(m.channel_b == "F4");
    assert(m.coherence_measure == CoherenceMeasure::MagnitudeSquared);
  }

  {
    const auto m = parse_nf_metric_spec("msc:alpha:F3:F4");
    assert(m.type == NfMetricSpec::Type::Coherence);
    assert(m.band == "alpha");
    assert(m.channel_a == "F3");
    assert(m.channel_b == "F4");
    assert(m.coherence_measure == CoherenceMeasure::MagnitudeSquared);
  }

  {
    const auto m = parse_nf_metric_spec("imcoh:alpha:F3:F4");
    assert(m.type == NfMetricSpec::Type::Coherence);
    assert(m.band == "alpha");
    assert(m.channel_a == "F3");
    assert(m.channel_b == "F4");
    assert(m.coherence_measure == CoherenceMeasure::ImaginaryCoherencyAbs);
  }

  {
    const auto m = parse_nf_metric_spec("coh:imcoh:alpha:F3:F4");
    assert(m.type == NfMetricSpec::Type::Coherence);
    assert(m.band == "alpha");
    assert(m.channel_a == "F3");
    assert(m.channel_b == "F4");
    assert(m.coherence_measure == CoherenceMeasure::ImaginaryCoherencyAbs);
  }

  {
    const auto m = parse_nf_metric_spec("pac:theta:gamma:Cz");
    assert(m.type == NfMetricSpec::Type::Pac);
    assert(m.phase_band == "theta");
    assert(m.amp_band == "gamma");
    assert(m.channel == "Cz");
    assert(m.pac_method == PacMethod::ModulationIndex);
  }

  {
    const auto m = parse_nf_metric_spec("mvl:theta:gamma:Cz");
    assert(m.type == NfMetricSpec::Type::Pac);
    assert(m.phase_band == "theta");
    assert(m.amp_band == "gamma");
    assert(m.channel == "Cz");
    assert(m.pac_method == PacMethod::MeanVectorLength);
  }

  bool threw = false;
  try {
    (void)parse_nf_metric_spec("bad");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(threw);

  std::cout << "test_nf_metric_spec passed\n";
  return 0;
}
