#pragma once

#include "qeeg/coherence.hpp"
#include "qeeg/pac.hpp"

#include <string>

namespace qeeg {

// Neurofeedback metric specification used by qeeg_nf_cli.
//
// Supported strings:
//  - alpha:Pz
//  - alpha/beta:Pz
//  - band:alpha:Pz
//  - ratio:alpha:beta:Pz
//  - asym:alpha:F4:F3           (log power ratio / asymmetry: log10(P(F4)/P(F3)))
//  - asymmetry:alpha:F4:F3      (alias for asym:)
//  - coh:alpha:F3:F4            (magnitude-squared coherence)
//  - coh:imcoh:alpha:F3:F4      (explicit measure)
//  - imcoh:alpha:F3:F4          (imaginary coherency, abs(imag(coherency)))
//  - msc:alpha:F3:F4            (magnitude-squared coherence)
//  - pac:theta:gamma:Cz         (Tort MI)
//  - mvl:theta:gamma:Cz         (mean vector length)
struct NfMetricSpec {
  enum class Type { Band, Ratio, Asymmetry, Coherence, Pac };
  Type type{Type::Band};

  // Band (and coherence/asymmetry) selection.
  std::string band;

  // Ratio bands.
  std::string band_num;
  std::string band_den;

  // Band/ratio/PAC channel.
  std::string channel;

  // Channel pair (coherence and asymmetry).
  std::string channel_a;
  std::string channel_b;
  CoherenceMeasure coherence_measure{CoherenceMeasure::MagnitudeSquared};

  // PAC (phase-amplitude coupling)
  PacMethod pac_method{PacMethod::ModulationIndex};
  std::string phase_band;
  std::string amp_band;
};

NfMetricSpec parse_nf_metric_spec(const std::string& s);

} // namespace qeeg
