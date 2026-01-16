#include "qeeg/nf_protocols.hpp"

#include "qeeg/utils.hpp"

#include <stdexcept>

namespace qeeg {

namespace {

static std::string norm_name(const std::string& s) {
  return to_lower(trim(s));
}

static void replace_all(std::string* s, const std::string& from, const std::string& to) {
  if (!s) return;
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = s->find(from, pos)) != std::string::npos) {
    s->replace(pos, from.size(), to);
    pos += to.size();
  }
}

static std::string apply_placeholders(std::string templ,
                                      const std::string& ch,
                                      const std::string& a,
                                      const std::string& b,
                                      const std::string& label) {
  const bool need_ch = (templ.find("{ch}") != std::string::npos) ||
                       (templ.find("{channel}") != std::string::npos);
  const bool need_a = (templ.find("{a}") != std::string::npos);
  const bool need_b = (templ.find("{b}") != std::string::npos);

  if (need_ch && ch.empty()) {
    throw std::runtime_error(label + " requires a channel; use --protocol-ch to override");
  }
  if (need_a && a.empty()) {
    throw std::runtime_error(label + " requires channel A; use --protocol-a to override");
  }
  if (need_b && b.empty()) {
    throw std::runtime_error(label + " requires channel B; use --protocol-b to override");
  }

  replace_all(&templ, "{ch}", ch);
  replace_all(&templ, "{channel}", ch);
  replace_all(&templ, "{a}", a);
  replace_all(&templ, "{b}", b);
  return templ;
}

} // namespace

std::vector<NfProtocolPreset> built_in_nf_protocols() {
  // NOTE: These presets are intentionally conservative and dependency-light.
  // They are meant as *examples* and quick-starts for qeeg_nf_cli.
  //
  // Band edges vary across labs and devices; users should verify choices
  // against their intended protocol and sampling constraints.
  std::vector<NfProtocolPreset> out;
  out.reserve(24);

  {
    NfProtocolPreset p;
    p.name = "alpha_up_pz";
    p.title = "Alpha uptraining";
    p.description = "Reward increased alpha (8-12 Hz) bandpower at Pz.";
    p.metric_template = "alpha:{ch}";
    p.default_channel = "Pz";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "theta_down_cz";
    p.title = "Theta downtraining";
    p.description = "Reward reduced theta (4-8 Hz) bandpower at Cz.";
    p.metric_template = "theta:{ch}";
    p.default_channel = "Cz";
    // Explicit theta edge (4-8) to match common NF conventions.
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Below;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "tbr_down_cz";
    p.title = "Theta/Beta ratio downtraining";
    p.description = "Reward a lower theta/beta ratio at Cz (theta 4-8 over beta 13-20).";
    p.metric_template = "theta/beta:{ch}";
    p.default_channel = "Cz";
    // Use an explicit beta band edge commonly used in TBR NF literature.
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-20,beta2:20-30,gamma:30-80";
    p.reward_direction = RewardDirection::Below;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "smr_up_cz";
    p.title = "SMR uptraining";
    p.description = "Reward increased SMR (12-15 Hz) bandpower at Cz.";
    p.metric_template = "smr:{ch}";
    p.default_channel = "Cz";
    // Add SMR as an explicit band.
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,smr:12-15,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "hibeta_down_fz";
    p.title = "High beta downtraining";
    p.description = "Reward reduced high beta (22-36 Hz) bandpower at Fz.";
    p.metric_template = "hibeta:{ch}";
    p.default_channel = "Fz";
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-21,hibeta:22-36,gamma:30-80";
    p.reward_direction = RewardDirection::Below;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "alpha_coh_up_f3_f4";
    p.title = "Alpha coherence uptraining";
    p.description = "Reward increased alpha-band coherence between F3 and F4.";
    p.metric_template = "coh:alpha:{a}:{b}";
    p.default_channel_a = "F3";
    p.default_channel_b = "F4";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    // Coherence can be noisier in short windows; a small smooth helps.
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "alpha_theta_ratio_up_pz";
    p.title = "Alpha/Theta ratio uptraining";
    p.description = "Reward increased alpha/theta ratio at Pz.";
    p.metric_template = "alpha/theta:{ch}";
    p.default_channel = "Pz";
    // Explicit theta edge (4-8) so the ratio matches typical protocol definitions.
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "pac_theta_gamma_up_cz";
    p.title = "Theta->Gamma PAC uptraining";
    p.description = "Reward increased theta-phase to gamma-amplitude PAC at Cz (Tort MI).";
    p.metric_template = "pac:theta:gamma:{ch}";
    p.default_channel = "Cz";
    // Explicit theta edge (4-8) for PAC phase band.
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    // PAC estimates typically need longer windows.
    p.window_seconds = 4.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }


  {
    NfProtocolPreset p;
    p.name = "alpha_up_oz";
    p.title = "Alpha uptraining (occipital)";
    p.description = "Reward increased alpha (8-12 Hz) bandpower at Oz.";
    p.metric_template = "alpha:{ch}";
    p.default_channel = "Oz";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "smr_up_c3";
    p.title = "SMR uptraining (C3)";
    p.description = "Reward increased SMR (12-15 Hz) bandpower at C3.";
    p.metric_template = "smr:{ch}";
    p.default_channel = "C3";
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,smr:12-15,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "smr_up_c4";
    p.title = "SMR uptraining (C4)";
    p.description = "Reward increased SMR (12-15 Hz) bandpower at C4.";
    p.metric_template = "smr:{ch}";
    p.default_channel = "C4";
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,smr:12-15,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "beta1_up_cz";
    p.title = "Beta1 uptraining";
    p.description = "Reward increased beta1 (15-18 Hz) bandpower at Cz.";
    p.metric_template = "beta1:{ch}";
    p.default_channel = "Cz";
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta1:15-18,beta2:18-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "theta_alpha_ratio_down_pz";
    p.title = "Theta/Alpha ratio downtraining";
    p.description = "Reward a lower theta/alpha ratio at Pz.";
    p.metric_template = "theta/alpha:{ch}";
    p.default_channel = "Pz";
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Below;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "imcoh_alpha_up_f3_f4";
    p.title = "Alpha imaginary coherency uptraining";
    p.description = "Reward increased alpha-band imaginary coherency between F3 and F4.";
    p.metric_template = "imcoh:alpha:{a}:{b}";
    p.default_channel_a = "F3";
    p.default_channel_b = "F4";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  {
    NfProtocolPreset p;
    p.name = "mvl_theta_gamma_up_cz";
    p.title = "Theta->Gamma coupling uptraining (MVL)";
    p.description = "Reward increased theta-phase to gamma-amplitude coupling at Cz (MVL).";
    p.metric_template = "mvl:theta:gamma:{ch}";
    p.default_channel = "Cz";
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 4.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }


  {
    NfProtocolPreset p;
    p.name = "alpha_asym_f4_f3";
    p.title = "Alpha asymmetry (F4/F3)";
    p.description = "Reward increased alpha-band asymmetry computed as log-power ratio between F4 and F3 (log10(P(F4)/P(F3))).";
    p.metric_template = "asym:alpha:{a}:{b}";
    p.default_channel_a = "F4";
    p.default_channel_b = "F3";
    // Provide a complete band spec so the preset is self-contained.
    p.band_spec = "delta:0.5-4,theta:4-8,alpha:8-12,beta:13-30,gamma:30-80";
    p.reward_direction = RewardDirection::Above;
    p.target_reward_rate = 0.6;
    p.window_seconds = 2.0;
    p.update_seconds = 0.25;
    p.metric_smooth_seconds = 0.5;
    out.push_back(p);
  }

  return out;
}

std::optional<NfProtocolPreset> find_nf_protocol_preset(const std::string& name) {
  const std::string key = norm_name(name);
  if (key.empty()) return std::nullopt;
  const auto presets = built_in_nf_protocols();
  for (const auto& p : presets) {
    if (norm_name(p.name) == key) return p;
  }
  return std::nullopt;
}

std::string nf_render_protocol_metric(const NfProtocolPreset& p,
                                      const std::string& channel_override,
                                      const std::string& channel_a_override,
                                      const std::string& channel_b_override) {
  const std::string ch = !channel_override.empty() ? channel_override : p.default_channel;
  const std::string a = !channel_a_override.empty() ? channel_a_override : p.default_channel_a;
  const std::string b = !channel_b_override.empty() ? channel_b_override : p.default_channel_b;
  return apply_placeholders(p.metric_template, ch, a, b, "protocol '" + p.name + "' metric");
}

std::string nf_render_protocol_bands(const NfProtocolPreset& p,
                                     const std::string& channel_override,
                                     const std::string& channel_a_override,
                                     const std::string& channel_b_override) {
  if (p.band_spec.empty()) return p.band_spec;
  const std::string ch = !channel_override.empty() ? channel_override : p.default_channel;
  const std::string a = !channel_a_override.empty() ? channel_a_override : p.default_channel_a;
  const std::string b = !channel_b_override.empty() ? channel_b_override : p.default_channel_b;
  return apply_placeholders(p.band_spec, ch, a, b, "protocol '" + p.name + "' bands");
}

} // namespace qeeg
