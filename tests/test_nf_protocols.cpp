#include "qeeg/nf_protocols.hpp"

#include "qeeg/bandpower.hpp"
#include "qeeg/nf_metric.hpp"
#include "qeeg/utils.hpp"

#include <cassert>
#include <iostream>
#include <unordered_set>
#include <vector>

static bool has_band(const std::vector<qeeg::BandDefinition>& bands, const std::string& name) {
  for (const auto& b : bands) {
    if (qeeg::to_lower(qeeg::trim(b.name)) == qeeg::to_lower(qeeg::trim(name))) return true;
  }
  return false;
}

int main() {
  const auto presets = qeeg::built_in_nf_protocols();
  assert(!presets.empty());

  // Names should be unique (case-insensitive).
  {
    std::unordered_set<std::string> seen;
    for (const auto& p : presets) {
      const std::string key = qeeg::to_lower(qeeg::trim(p.name));
      assert(!key.empty());
      const bool ok = seen.insert(key).second;
      assert(ok && "Duplicate protocol preset name");
    }
  }

  // Lookup should be case-insensitive.
  {
    const auto p = qeeg::find_nf_protocol_preset("SMR_UP_CZ");
    assert(p.has_value());
    assert(p->name == "smr_up_cz");
  }

  // Placeholder substitution.
  {
    const auto p = qeeg::find_nf_protocol_preset("alpha_up_pz");
    assert(p.has_value());
    const std::string m_default = qeeg::nf_render_protocol_metric(*p);
    assert(m_default == "alpha:Pz");
    const std::string m_ov = qeeg::nf_render_protocol_metric(*p, "O1");
    assert(m_ov == "alpha:O1");
  }

  {
    const auto p = qeeg::find_nf_protocol_preset("alpha_coh_up_f3_f4");
    assert(p.has_value());
    const std::string m_default = qeeg::nf_render_protocol_metric(*p);
    assert(m_default == "coh:alpha:F3:F4");
    const std::string m_ov = qeeg::nf_render_protocol_metric(*p, "", "C3", "C4");
    assert(m_ov == "coh:alpha:C3:C4");
  }

  // Self-consistency: every built-in preset should render to a parseable metric,
  // and any referenced bands should exist in the preset band list (or defaults).
  for (const auto& p : presets) {
    const std::string metric_s = qeeg::nf_render_protocol_metric(p);
    const std::string bands_s = qeeg::nf_render_protocol_bands(p); // may be empty

    const qeeg::NfMetricSpec ms = qeeg::parse_nf_metric_spec(metric_s);
    const auto bands = qeeg::parse_band_spec(bands_s);
    assert(!bands.empty());

    switch (ms.type) {
      case qeeg::NfMetricSpec::Type::Band:
        assert(!ms.band.empty());
        assert(has_band(bands, ms.band));
        break;
      case qeeg::NfMetricSpec::Type::Ratio:
        assert(!ms.band_num.empty());
        assert(!ms.band_den.empty());
        assert(has_band(bands, ms.band_num));
        assert(has_band(bands, ms.band_den));
        break;

      case qeeg::NfMetricSpec::Type::Asymmetry: {
        const bool has_band = !ms.band.empty();
        const bool has_ch = !ms.channel_a.empty() && !ms.channel_b.empty();
        // Ensure referenced band exists in band spec.
        bool band_ok = false;
        for (const auto& b : bands) {
          if (b.name == ms.band) { band_ok = true; break; }
        }
        assert(has_band && has_ch && band_ok);
        break;
      }
      case qeeg::NfMetricSpec::Type::Coherence:
        assert(!ms.band.empty());
        assert(has_band(bands, ms.band));
        break;
      case qeeg::NfMetricSpec::Type::Pac:
        assert(!ms.phase_band.empty());
        assert(!ms.amp_band.empty());
        assert(has_band(bands, ms.phase_band));
        assert(has_band(bands, ms.amp_band));
        break;
      default:
        assert(false && "Unknown NfMetricSpec::Type");
        break;
    }
  }

  std::cout << "ok\n";
  return 0;
}
