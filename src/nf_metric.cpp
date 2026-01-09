#include "qeeg/nf_metric.hpp"
#include "qeeg/utils.hpp"

#include <stdexcept>

namespace qeeg {

NfMetricSpec parse_nf_metric_spec(const std::string& s) {
  // Supported:
  //  - alpha:Pz
  //  - alpha/beta:Pz
  //  - band:alpha:Pz
  //  - ratio:alpha:beta:Pz
  //  - coh:alpha:F3:F4
  //  - coh:imcoh:alpha:F3:F4
  //  - msc:alpha:F3:F4
  //  - imcoh:alpha:F3:F4
  //  - pac:theta:gamma:Cz  (Tort MI)
  //  - mvl:theta:gamma:Cz  (mean vector length)
  const auto parts = split(trim(s), ':');
  if (parts.empty()) throw std::runtime_error("--metric: empty spec");

  // Long-form.
  if (!parts.empty()) {
    const std::string head = to_lower(trim(parts[0]));

    if (head == "band") {
      if (parts.size() != 3) throw std::runtime_error("--metric band: expects band:NAME:CHANNEL");
      NfMetricSpec m;
      m.type = NfMetricSpec::Type::Band;
      m.band = trim(parts[1]);
      m.channel = trim(parts[2]);
      return m;
    }

    if (head == "ratio") {
      if (parts.size() != 4) throw std::runtime_error("--metric ratio: expects ratio:NUM:DEN:CHANNEL");
      NfMetricSpec m;
      m.type = NfMetricSpec::Type::Ratio;
      m.band_num = trim(parts[1]);
      m.band_den = trim(parts[2]);
      m.channel = trim(parts[3]);
      return m;
    }

    if (head == "coh" || head == "coherence") {
      if (parts.size() != 4 && parts.size() != 5) {
        throw std::runtime_error("--metric coh: expects coh:BAND:CH_A:CH_B or coh:MEASURE:BAND:CH_A:CH_B");
      }
      NfMetricSpec m;
      m.type = NfMetricSpec::Type::Coherence;
      size_t idx = 1;
      if (parts.size() == 5) {
        m.coherence_measure = parse_coherence_measure_token(trim(parts[1]));
        idx = 2;
      }
      m.band = trim(parts[idx + 0]);
      m.channel_a = trim(parts[idx + 1]);
      m.channel_b = trim(parts[idx + 2]);
      return m;
    }

    // Convenience: allow measure token as the head.
    if (head == "msc" || head == "imcoh" || head == "absimag") {
      if (parts.size() != 4) throw std::runtime_error("--metric: expects MEASURE:BAND:CH_A:CH_B");
      NfMetricSpec m;
      m.type = NfMetricSpec::Type::Coherence;
      m.coherence_measure = parse_coherence_measure_token(head);
      m.band = trim(parts[1]);
      m.channel_a = trim(parts[2]);
      m.channel_b = trim(parts[3]);
      return m;
    }

    if (head == "pac" || head == "pacmi") {
      if (parts.size() != 4) throw std::runtime_error("--metric pac: expects pac:PHASE:AMP:CHANNEL");
      NfMetricSpec m;
      m.type = NfMetricSpec::Type::Pac;
      m.pac_method = PacMethod::ModulationIndex;
      m.phase_band = trim(parts[1]);
      m.amp_band = trim(parts[2]);
      m.channel = trim(parts[3]);
      return m;
    }

    if (head == "mvl" || head == "pacmvl") {
      if (parts.size() != 4) throw std::runtime_error("--metric mvl: expects mvl:PHASE:AMP:CHANNEL");
      NfMetricSpec m;
      m.type = NfMetricSpec::Type::Pac;
      m.pac_method = PacMethod::MeanVectorLength;
      m.phase_band = trim(parts[1]);
      m.amp_band = trim(parts[2]);
      m.channel = trim(parts[3]);
      return m;
    }
  }

  // Short-form (bandpower or ratio)
  if (parts.size() != 2) {
    throw std::runtime_error(
      "--metric: expected 'alpha:Pz', 'alpha/beta:Pz', 'coh:alpha:F3:F4', 'imcoh:alpha:F3:F4', or 'pac:theta:gamma:Cz'");
  }

  NfMetricSpec m;
  const std::string left = trim(parts[0]);
  m.channel = trim(parts[1]);
  const auto slash = left.find('/');
  if (slash == std::string::npos) {
    m.type = NfMetricSpec::Type::Band;
    m.band = left;
  } else {
    m.type = NfMetricSpec::Type::Ratio;
    m.band_num = trim(left.substr(0, slash));
    m.band_den = trim(left.substr(slash + 1));
  }
  return m;
}

} // namespace qeeg
