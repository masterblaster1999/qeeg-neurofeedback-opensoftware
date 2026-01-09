#include "qeeg/artifacts.hpp"

#include "qeeg/robust_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qeeg {
namespace {

struct WindowRaw {
  double t_start_sec{0.0};
  double t_end_sec{0.0};
  std::vector<double> ptp;
  std::vector<double> rms;
  std::vector<double> kurt;
};

} // namespace

ArtifactDetectionResult detect_artifacts(const EEGRecording& rec, const ArtifactDetectionOptions& opt) {
  if (rec.fs_hz <= 0.0) throw std::runtime_error("detect_artifacts: invalid sampling rate");
  if (rec.n_channels() == 0 || rec.n_samples() == 0) {
    throw std::runtime_error("detect_artifacts: empty recording");
  }
  if (!(opt.window_seconds > 0.0) || !(opt.step_seconds > 0.0)) {
    throw std::runtime_error("detect_artifacts: window_seconds and step_seconds must be > 0");
  }
  if (opt.min_bad_channels < 1) {
    throw std::runtime_error("detect_artifacts: min_bad_channels must be >= 1");
  }

  const double fs = rec.fs_hz;
  const size_t win_n = static_cast<size_t>(std::llround(opt.window_seconds * fs));
  const size_t step_n = static_cast<size_t>(std::llround(opt.step_seconds * fs));
  if (win_n < 2) throw std::runtime_error("detect_artifacts: window too small");
  if (step_n < 1) throw std::runtime_error("detect_artifacts: step too small");
  if (step_n > win_n) throw std::runtime_error("detect_artifacts: step_seconds must be <= window_seconds");

  const size_t n_ch = rec.n_channels();
  const size_t n_samp = rec.n_samples();

  const size_t baseline_end = (opt.baseline_seconds > 0.0)
    ? std::min(n_samp, static_cast<size_t>(std::llround(opt.baseline_seconds * fs)))
    : n_samp;

  // First pass: compute raw features for every window.
  std::vector<WindowRaw> raw;
  for (size_t start = 0; start + win_n <= n_samp; start += step_n) {
    WindowRaw w;
    w.t_start_sec = static_cast<double>(start) / fs;
    w.t_end_sec = static_cast<double>(start + win_n) / fs;
    w.ptp.assign(n_ch, 0.0);
    w.rms.assign(n_ch, 0.0);
    w.kurt.assign(n_ch, 0.0);

    for (size_t ch = 0; ch < n_ch; ++ch) {
      const auto& x = rec.data[ch];
      double mn = std::numeric_limits<double>::infinity();
      double mx = -std::numeric_limits<double>::infinity();
      double s1 = 0.0;
      double s2 = 0.0;
      double s3 = 0.0;
      double s4 = 0.0;
      for (size_t i = start; i < start + win_n; ++i) {
        const double v = static_cast<double>(x[i]);
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        s1 += v;
        const double v2 = v * v;
        s2 += v2;
        s3 += v2 * v;
        s4 += v2 * v2;
      }
      const double n = static_cast<double>(win_n);
      const double mean = s1 / n;
      const double ex2 = s2 / n;
      const double ex3 = s3 / n;
      const double ex4 = s4 / n;
      const double var = std::max(0.0, ex2 - mean * mean);
      const double rms = std::sqrt(std::max(0.0, ex2));

      // Fourth central moment using raw moments.
      // mu4 = E[(x-mean)^4] = E[x^4] -4 mean E[x^3] +6 mean^2 E[x^2] -3 mean^4
      const double mu4 = ex4 - 4.0 * mean * ex3 + 6.0 * mean * mean * ex2 - 3.0 * std::pow(mean, 4);
      double kurt_excess = 0.0;
      if (var > 1e-24) {
        kurt_excess = (mu4 / (var * var)) - 3.0;
      }

      w.ptp[ch] = mx - mn;
      w.rms[ch] = rms;
      w.kurt[ch] = kurt_excess;
    }

    raw.push_back(std::move(w));
  }

  if (raw.empty()) {
    throw std::runtime_error("detect_artifacts: no windows (recording shorter than window?)");
  }

  // Collect baseline distributions per channel.
  std::vector<std::vector<double>> base_ptp(n_ch);
  std::vector<std::vector<double>> base_rms(n_ch);
  std::vector<std::vector<double>> base_kurt(n_ch);
  for (size_t wi = 0; wi < raw.size(); ++wi) {
    const size_t end_sample = static_cast<size_t>(std::llround(raw[wi].t_end_sec * fs));
    if (end_sample > baseline_end) break;
    for (size_t ch = 0; ch < n_ch; ++ch) {
      base_ptp[ch].push_back(raw[wi].ptp[ch]);
      base_rms[ch].push_back(raw[wi].rms[ch]);
      base_kurt[ch].push_back(raw[wi].kurt[ch]);
    }
  }

  // If the baseline window selection yielded nothing (very short recordings), fall back to all windows.
  bool baseline_empty = true;
  for (const auto& v : base_ptp) {
    if (!v.empty()) { baseline_empty = false; break; }
  }
  if (baseline_empty) {
    for (size_t wi = 0; wi < raw.size(); ++wi) {
      for (size_t ch = 0; ch < n_ch; ++ch) {
        base_ptp[ch].push_back(raw[wi].ptp[ch]);
        base_rms[ch].push_back(raw[wi].rms[ch]);
        base_kurt[ch].push_back(raw[wi].kurt[ch]);
      }
    }
  }

  ArtifactDetectionResult out;
  out.opt = opt;
  out.channel_names = rec.channel_names;
  out.baseline_stats.assign(n_ch, ArtifactChannelStats{});

  for (size_t ch = 0; ch < n_ch; ++ch) {
    std::vector<double> tmp;

    tmp = base_ptp[ch];
    const double ptp_med = median_inplace(&tmp);
    out.baseline_stats[ch].ptp_median = ptp_med;
    out.baseline_stats[ch].ptp_scale = robust_scale(base_ptp[ch], ptp_med);

    tmp = base_rms[ch];
    const double rms_med = median_inplace(&tmp);
    out.baseline_stats[ch].rms_median = rms_med;
    out.baseline_stats[ch].rms_scale = robust_scale(base_rms[ch], rms_med);

    tmp = base_kurt[ch];
    const double k_med = median_inplace(&tmp);
    out.baseline_stats[ch].kurtosis_median = k_med;
    out.baseline_stats[ch].kurtosis_scale = robust_scale(base_kurt[ch], k_med);
  }

  // Second pass: compute z-scores and flags.
  out.windows.reserve(raw.size());
  size_t bad_wins = 0;

  for (size_t wi = 0; wi < raw.size(); ++wi) {
    ArtifactWindowResult wr;
    wr.t_start_sec = raw[wi].t_start_sec;
    wr.t_end_sec = raw[wi].t_end_sec;
    wr.channels.assign(n_ch, ArtifactChannelMetrics{});

    size_t bad_ch = 0;
    for (size_t ch = 0; ch < n_ch; ++ch) {
      ArtifactChannelMetrics cm;
      cm.ptp = raw[wi].ptp[ch];
      cm.rms = raw[wi].rms[ch];
      cm.kurtosis = raw[wi].kurt[ch];

      const auto& st = out.baseline_stats[ch];
      cm.ptp_z = (cm.ptp - st.ptp_median) / st.ptp_scale;
      cm.rms_z = (cm.rms - st.rms_median) / st.rms_scale;
      cm.kurtosis_z = (cm.kurtosis - st.kurtosis_median) / st.kurtosis_scale;

      bool bad = false;
      if (opt.ptp_z > 0.0 && cm.ptp_z > opt.ptp_z) bad = true;
      if (opt.rms_z > 0.0 && cm.rms_z > opt.rms_z) bad = true;
      if (opt.kurtosis_z > 0.0 && cm.kurtosis_z > opt.kurtosis_z) bad = true;
      cm.bad = bad;
      if (bad) ++bad_ch;

      wr.channels[ch] = cm;
    }

    wr.bad_channel_count = bad_ch;
    wr.bad = (bad_ch >= opt.min_bad_channels);
    if (wr.bad) ++bad_wins;
    out.windows.push_back(std::move(wr));
  }

  out.total_bad_windows = bad_wins;
  return out;
}

} // namespace qeeg
