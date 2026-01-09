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
//  - coh:alpha:F3:F4            (magnitude-squared coherence)
//  - coh:imcoh:alpha:F3:F4      (explicit measure)
//  - imcoh:alpha:F3:F4          (imaginary coherency, abs(imag(coherency)))
//  - msc:alpha:F3:F4            (magnitude-squared coherence)
//  - pac:theta:gamma:Cz         (Tort MI)
//  - mvl:theta:gamma:Cz         (mean vector length)
struct NfMetricSpec {
  enum class Type { Band, Ratio, Coherence, Pac };
  Type type{Type::Band};

  // Band (and coherence) selection.
  std::string band;

  // Ratio bands.
  std::string band_num;
  std::string band_den;

  // Band/ratio channel.
  std::string channel;

  // Coherence pair.
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
