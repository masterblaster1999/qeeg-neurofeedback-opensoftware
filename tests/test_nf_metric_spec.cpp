#include "qeeg/nf_metric.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
  using namespace qeeg;

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
    assert(m.coherence_measure == CoherenceMeasure::MagnitudeSquared);
  }

  {
    const auto m = parse_nf_metric_spec("imcoh:alpha:F3:F4");
    assert(m.type == NfMetricSpec::Type::Coherence);
    assert(m.coherence_measure == CoherenceMeasure::ImaginaryCoherencyAbs);
  }

  {
    const auto m = parse_nf_metric_spec("coh:imcoh:alpha:F3:F4");
    assert(m.type == NfMetricSpec::Type::Coherence);
    assert(m.coherence_measure == CoherenceMeasure::ImaginaryCoherencyAbs);
  }

  {
    const auto m = parse_nf_metric_spec("pac:theta:gamma:Cz");
    assert(m.type == NfMetricSpec::Type::Pac);
    assert(m.pac_method == PacMethod::ModulationIndex);
    assert(m.phase_band == "theta");
    assert(m.amp_band == "gamma");
    assert(m.channel == "Cz");
  }

  {
    const auto m = parse_nf_metric_spec("mvl:theta:gamma:Cz");
    assert(m.type == NfMetricSpec::Type::Pac);
    assert(m.pac_method == PacMethod::MeanVectorLength);
  }

  {
    bool ok = false;
    try {
      (void)parse_nf_metric_spec("coh:alpha:F3");
    } catch (const std::exception&) {
      ok = true;
    }
    assert(ok);
  }

  std::cout << "NF metric spec parsing tests passed.\n";
  return 0;
}
