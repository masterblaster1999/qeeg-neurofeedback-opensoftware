#include "qeeg/spectral_features.hpp"

#include "qeeg/preprocess.hpp"
#include "qeeg/reader.hpp"
#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"
#include "qeeg/welch_psd.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qeeg;

namespace {


enum class AperiodicBackgroundMode {
  LogLog,
  TwoSlope,
  Knee,
  AutoAIC,
  AutoBIC,
};

struct Args {
  std::string input_path;
  std::string outdir{"out_spectral"};

  // Recording
  double fs_csv{0.0}; // only for CSV inputs; 0 = infer from time column

  // PSD
  size_t nperseg{1024};
  double overlap{0.5};

  // Feature range
  double fmin_hz{1.0};
  double fmax_hz{40.0};

  // Alpha peak search range
  double alpha_min_hz{8.0};
  double alpha_max_hz{12.0};

  // Aperiodic (1/f-like) fit range in Hz. If not provided, defaults to --range.
  bool aperiodic_range_set{false};
  double aperiodic_min_hz{0.0};
  double aperiodic_max_hz{0.0};

  // Frequency ranges to exclude from the aperiodic (log-log) fit (repeatable).
  std::vector<FrequencyRange> aperiodic_excludes;

  // Optional "two-slope" aperiodic fit with an estimated knee frequency.
  bool include_aperiodic_two_slope{true};
  std::size_t aperiodic_two_slope_min_points_per_side{6};

  // Optional curved aperiodic knee model fit (offset - log10(knee + f^exponent)).
  // This yields SpecParam/FOOOF-style parameters: offset, knee, exponent, and fit quality.
  bool include_aperiodic_knee_model{true};

  // Aperiodic background model used for prominence / periodic residual metrics.
  // Default preserves legacy behavior (loglog). Auto modes select per-channel
  // by information criteria among enabled models.
  AperiodicBackgroundMode aperiodic_background{AperiodicBackgroundMode::LogLog};

  // Optional bandpower/ratio outputs (commonly used in neurofeedback summaries).
  bool include_bands{true};
  bool include_ratios{true};
  bool include_periodic_bands{true};
  bool include_band_peaks{true};

  struct BandDef {
    std::string name;      // user-facing name
    std::string key;       // sanitized base key (lowercase, underscore)
    std::string col_power; // e.g. theta_power
    std::string col_rel;   // e.g. theta_rel
    std::string col_periodic_power; // e.g. theta_periodic_power
    std::string col_periodic_rel;   // e.g. theta_periodic_rel
    std::string col_periodic_frac;  // e.g. theta_periodic_frac
    // Per-band prominent peak features (max prominence above aperiodic fit).
    std::string col_prominent_peak_hz;           // e.g. theta_prominent_peak_hz
    std::string col_prominent_peak_hz_refined;   // e.g. theta_prominent_peak_hz_refined
    std::string col_prominent_peak_value_db;     // e.g. theta_prominent_peak_value_db
    std::string col_prominent_peak_fwhm_hz;      // e.g. theta_prominent_peak_fwhm_hz
    std::string col_prominent_peak_q;            // e.g. theta_prominent_peak_q
    std::string col_prominent_peak_prominence_db; // e.g. theta_prominent_peak_prominence_db
    double lo_hz{0.0};
    double hi_hz{0.0};
  };
  struct RatioDef {
    std::string col; // column name, e.g. theta_beta
    std::string num_key;
    std::string den_key;
  };

  std::vector<BandDef> bands;   // empty => defaults
  std::vector<RatioDef> ratios; // empty => defaults (if possible)
  bool bands_custom{false};
  bool ratios_custom{false};

  // Spectral edge fractions, e.g. 0.95 => SEF95.
  //
  // This is repeatable on the CLI via --edge X. The first --edge clears the
  // default list.
  std::vector<double> edges{0.95};
  bool edges_custom{false};

  // Optional preprocessing
  bool average_reference{false};
  double notch_hz{0.0};
  double notch_q{30.0};
  double bandpass_low_hz{0.0};
  double bandpass_high_hz{0.0};
  bool zero_phase{false};
};

static std::string fmt_double(double v, int precision = 6) {
  if (!std::isfinite(v)) return "NaN";
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << v;
  return oss.str();
}

struct InfoCriteria {
  double aic{std::numeric_limits<double>::quiet_NaN()};
  double bic{std::numeric_limits<double>::quiet_NaN()};
};

static InfoCriteria info_criteria_from_rmse(double rmse_unweighted,
                                           std::size_t n_points,
                                           int num_params) {
  InfoCriteria out;
  if (num_params < 1) return out;
  if (n_points < 1) return out;
  if (!std::isfinite(rmse_unweighted)) return out;
  const double n = static_cast<double>(n_points);
  // RSS in the log10(PSD) domain. Add a tiny floor for numerical stability.
  const double rss = std::max(1e-24, rmse_unweighted * rmse_unweighted * n);
  const double sigma2 = std::max(1e-24, rss / n);

  // AIC/BIC up to an additive constant (which cancels in model comparisons).
  const double ll_term = n * std::log(sigma2);
  out.aic = ll_term + 2.0 * static_cast<double>(num_params);
  out.bic = ll_term + static_cast<double>(num_params) * std::log(n);
  return out;
}

static std::string best_model_from_score(double score_loglog,
                                        double score_two_slope,
                                        double score_knee) {
  double best = std::numeric_limits<double>::infinity();
  std::string name = "na";
  if (std::isfinite(score_loglog) && score_loglog < best) {
    best = score_loglog;
    name = "loglog";
  }
  if (std::isfinite(score_two_slope) && score_two_slope < best) {
    best = score_two_slope;
    name = "two_slope";
  }
  if (std::isfinite(score_knee) && score_knee < best) {
    best = score_knee;
    name = "knee";
  }
  return name;
}


struct ModelDeltasWeights {
  double delta_loglog{std::numeric_limits<double>::quiet_NaN()};
  double delta_two_slope{std::numeric_limits<double>::quiet_NaN()};
  double delta_knee{std::numeric_limits<double>::quiet_NaN()};
  double weight_loglog{0.0};
  double weight_two_slope{0.0};
  double weight_knee{0.0};
};

// Compute Δ (relative to the best/lowest score) and normalized weights for up to
// three candidate models.
//
// Weights are computed as exp(-0.5 * Δ) normalized across the finite scores.
// This matches Akaike weights when the scores are AIC, and the analogous
// "BIC weights" when the scores are BIC.
static ModelDeltasWeights model_deltas_and_weights(double score_loglog,
                                                   double score_two_slope,
                                                   double score_knee) {
  ModelDeltasWeights out;
  double best = std::numeric_limits<double>::infinity();
  if (std::isfinite(score_loglog) && score_loglog < best) best = score_loglog;
  if (std::isfinite(score_two_slope) && score_two_slope < best) best = score_two_slope;
  if (std::isfinite(score_knee) && score_knee < best) best = score_knee;

  if (!std::isfinite(best)) {
    out.weight_loglog = std::numeric_limits<double>::quiet_NaN();
    out.weight_two_slope = std::numeric_limits<double>::quiet_NaN();
    out.weight_knee = std::numeric_limits<double>::quiet_NaN();
    return out;
  }

  double wsum = 0.0;
  if (std::isfinite(score_loglog)) {
    out.delta_loglog = score_loglog - best;
    const double w = std::exp(-0.5 * std::max(0.0, out.delta_loglog));
    out.weight_loglog = std::isfinite(w) ? w : 0.0;
    wsum += out.weight_loglog;
  }
  if (std::isfinite(score_two_slope)) {
    out.delta_two_slope = score_two_slope - best;
    const double w = std::exp(-0.5 * std::max(0.0, out.delta_two_slope));
    out.weight_two_slope = std::isfinite(w) ? w : 0.0;
    wsum += out.weight_two_slope;
  }
  if (std::isfinite(score_knee)) {
    out.delta_knee = score_knee - best;
    const double w = std::exp(-0.5 * std::max(0.0, out.delta_knee));
    out.weight_knee = std::isfinite(w) ? w : 0.0;
    wsum += out.weight_knee;
  }

  if (!(wsum > 0.0) || !std::isfinite(wsum)) {
    out.weight_loglog = std::numeric_limits<double>::quiet_NaN();
    out.weight_two_slope = std::numeric_limits<double>::quiet_NaN();
    out.weight_knee = std::numeric_limits<double>::quiet_NaN();
    return out;
  }

  out.weight_loglog /= wsum;
  out.weight_two_slope /= wsum;
  out.weight_knee /= wsum;
  return out;
}

static std::string to_lower(std::string s) {
  for (char& ch : s) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return s;
}


static std::string aperiodic_background_mode_to_string(AperiodicBackgroundMode m) {
  switch (m) {
    case AperiodicBackgroundMode::LogLog: return "loglog";
    case AperiodicBackgroundMode::TwoSlope: return "two_slope";
    case AperiodicBackgroundMode::Knee: return "knee";
    case AperiodicBackgroundMode::AutoAIC: return "auto_aic";
    case AperiodicBackgroundMode::AutoBIC: return "auto_bic";
  }
  return "loglog";
}

static AperiodicBackgroundMode parse_aperiodic_background_mode(std::string s) {
  s = to_lower(std::move(s));
  for (char& ch : s) {
    if (ch == '-') ch = '_';
  }

  if (s == "loglog" || s == "log_log" || s == "single" || s == "single_slope") {
    return AperiodicBackgroundMode::LogLog;
  }
  if (s == "two_slope" || s == "twoslope" || s == "two_slope_loglog" || s == "two_slope_log_log") {
    return AperiodicBackgroundMode::TwoSlope;
  }
  if (s == "knee" || s == "knee_model" || s == "aperiodic_knee") {
    return AperiodicBackgroundMode::Knee;
  }
  if (s == "auto_aic" || s == "aic" || s == "auto") {
    return AperiodicBackgroundMode::AutoAIC;
  }
  if (s == "auto_bic" || s == "bic") {
    return AperiodicBackgroundMode::AutoBIC;
  }

  throw std::runtime_error("Unknown --aperiodic-background: " + s);
}

static std::string sanitize_key(std::string s) {
  s = to_lower(std::move(s));
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    out.push_back(ok ? ch : '_');
  }
  // Collapse consecutive underscores.
  std::string collapsed;
  collapsed.reserve(out.size());
  bool prev_us = false;
  for (char ch : out) {
    if (ch == '_') {
      if (!prev_us) collapsed.push_back(ch);
      prev_us = true;
    } else {
      collapsed.push_back(ch);
      prev_us = false;
    }
  }
  // Trim underscores.
  while (!collapsed.empty() && collapsed.front() == '_') collapsed.erase(collapsed.begin());
  while (!collapsed.empty() && collapsed.back() == '_') collapsed.pop_back();
  if (collapsed.empty()) collapsed = "band";
  if (collapsed.front() >= '0' && collapsed.front() <= '9') collapsed = "b_" + collapsed;
  return collapsed;
}

static std::vector<Args::BandDef> default_bands() {
  // Common EEG summary bands. Gamma upper edge is often reported as 45 Hz in QEEG outputs.
  // The analysis range (--range) still clamps these in practice.
  std::vector<Args::BandDef> b;
  auto add = [&](const std::string& name, double lo, double hi) {
    Args::BandDef d;
    d.name = name;
    d.key = sanitize_key(name);
    d.col_power = d.key + "_power";
    d.col_rel = d.key + "_rel";
    d.col_periodic_power = d.key + "_periodic_power";
    d.col_periodic_rel = d.key + "_periodic_rel";
    d.col_periodic_frac = d.key + "_periodic_frac";
    d.col_prominent_peak_hz = d.key + "_prominent_peak_hz";
    d.col_prominent_peak_hz_refined = d.key + "_prominent_peak_hz_refined";
    d.col_prominent_peak_value_db = d.key + "_prominent_peak_value_db";
    d.col_prominent_peak_fwhm_hz = d.key + "_prominent_peak_fwhm_hz";
    d.col_prominent_peak_q = d.key + "_prominent_peak_q";
    d.col_prominent_peak_prominence_db = d.key + "_prominent_peak_prominence_db";
    d.lo_hz = lo;
    d.hi_hz = hi;
    b.push_back(d);
  };
  add("delta", 1.0, 4.0);
  add("theta", 4.0, 8.0);
  add("alpha", 8.0, 12.0);
  add("beta", 12.0, 30.0);
  add("gamma", 30.0, 45.0);
  return b;
}

static std::vector<Args::RatioDef> default_ratios(const std::vector<Args::BandDef>& bands) {
  std::map<std::string, bool> have;
  for (const auto& b : bands) have[b.key] = true;

  std::vector<Args::RatioDef> r;
  auto add = [&](const std::string& col, const std::string& num, const std::string& den) {
    if (!have[num] || !have[den]) return;
    Args::RatioDef d;
    d.col = sanitize_key(col);
    d.num_key = num;
    d.den_key = den;
    r.push_back(d);
  };
  add("theta_beta", "theta", "beta");
  add("alpha_theta", "alpha", "theta");
  return r;
}

static void print_help() {
  std::cout
      << "qeeg_spectral_features_cli\n\n"
      << "Compute quick per-channel spectral summary features from Welch PSD.\n"
      << "Outputs a CSV + JSON sidecar + run manifest for qeeg_ui_cli.\n\n"
      << "Features (per channel):\n"
      << "  - total_power  : integral(PSD) over [fmin,fmax]\n"
      << "  - entropy      : normalized spectral entropy over [fmin,fmax] (0..1)\n"
      << "  - mean_hz      : power-weighted mean frequency (spectral centroid)\n"
      << "  - bandwidth_hz : spectral bandwidth (power-weighted std dev of frequency)\n"
      << "  - skewness     : spectral skewness of the power-weighted frequency distribution\n"
      << "  - kurtosis_excess : spectral excess kurtosis of the power-weighted frequency distribution\n"
      << "  - flatness     : spectral flatness (geometric_mean/arith_mean of PSD; 0..1)\n"
      << "  - peak_hz      : frequency of max PSD (simple argmax)\n"
      << "  - peak_hz_refined : peak frequency refined by quadratic (parabolic) interpolation\n"
      << "  - peak_value_db   : PSD value at peak_hz expressed in dB (10*log10)\n"
      << "  - peak_fwhm_hz    : full-width at half-maximum around peak_hz (within analysis range)\n"
      << "  - peak_q          : Q factor = peak_hz / peak_fwhm_hz\n"
      << "  - peak_prominence_db : peak prominence in dB vs the selected aperiodic background model (see --aperiodic-background)\n"
      << "  - prominent_peak_hz      : frequency of the most prominent oscillatory peak (max prominence vs selected aperiodic background)\n"
      << "  - prominent_peak_hz_refined : prominent peak frequency refined by quadratic (parabolic) interpolation\n"
      << "  - prominent_peak_value_db   : PSD value at prominent_peak_hz expressed in dB (10*log10)\n"
      << "  - prominent_peak_fwhm_hz    : full-width at half-maximum around prominent_peak_hz (within analysis range)\n"
      << "  - prominent_peak_q          : Q factor = prominent_peak_hz / prominent_peak_fwhm_hz\n"
      << "  - prominent_peak_prominence_db : prominence in dB at prominent_peak_hz vs the selected aperiodic background\n"
      << "  - alpha_peak_hz      : peak frequency within the alpha range (default 8-12 Hz)\n"
      << "  - alpha_peak_hz_refined : alpha peak refined by quadratic (parabolic) interpolation\n"
      << "  - alpha_peak_value_db   : PSD value at alpha_peak_hz expressed in dB (10*log10)\n"
      << "  - alpha_fwhm_hz         : full-width at half-maximum around alpha_peak_hz (within alpha range)\n"
      << "  - alpha_q               : Q factor = alpha_peak_hz / alpha_fwhm_hz\n"
      << "  - alpha_prominence_db: alpha peak prominence in dB vs the selected aperiodic background\n"
      << "  - median_hz    : spectral edge frequency at 50% cumulative power\n"
      << "  - sefXX_hz     : spectral edge frequency at edge% cumulative power (one column per --edge; default 95%)\n"
      << "  - periodic_median_hz : periodic SEF50 on residual power above the aperiodic fit\n"
      << "  - periodic_sefXX_hz  : periodic SEFXX on residual power above the aperiodic fit (matches --edge list)\n"
      << "  - aperiodic_offset   : log10(PSD) intercept of a log-log fit over --aperiodic-range\n"
      << "  - aperiodic_exponent : k in 1/f^k (=-slope of log10 PSD vs log10 f) over --aperiodic-range\n"
      << "  - aperiodic_r2       : R^2 goodness of the log-log fit (log10 domain) over --aperiodic-range\n"
      << "  - aperiodic_rmse     : RMSE of the log-log fit in log10(PSD) units\n"
      << "  - aperiodic_n_points : number of points used in the log-log fit\n"
      << "  - aperiodic_slope    : slope of log10(PSD) vs log10(f) (negative for 1/f^k)\n"
      << "  - aperiodic_offset_db: 10*aperiodic_offset (dB), approximately the predicted power at 1 Hz\n"
      << "  - aperiodic_aic    : Akaike Information Criterion (AIC) for the single-slope log-log aperiodic fit (unweighted residuals)\n"
      << "  - aperiodic_bic    : Bayesian Information Criterion (BIC) for the single-slope log-log aperiodic fit (unweighted residuals)\n"
      << "  - aperiodic_offset_knee : offset of a curved knee aperiodic model (disable with --no-aperiodic-knee-model)\n"
      << "  - aperiodic_exponent_knee: exponent (k) of the curved knee aperiodic model\n"
      << "  - aperiodic_knee_param   : knee parameter of the curved model (units Hz^k)\n"
      << "  - aperiodic_knee_freq_hz : knee frequency derived from knee_param^(1/k) (Hz)\n"
      << "  - aperiodic_r2_knee      : R^2 goodness of the curved knee model (log10 domain)\n"
      << "  - aperiodic_rmse_knee    : RMSE of the curved knee model in log10(PSD) units\n"
      << "  - aperiodic_n_points_knee: number of points used in the curved knee fit\n"
      << "  - aperiodic_aic_knee      : AIC for the curved aperiodic knee model (unweighted residuals)\n"
      << "  - aperiodic_bic_knee      : BIC for the curved aperiodic knee model (unweighted residuals)\n"
      << "  - aperiodic_knee_hz  : estimated knee frequency for a continuous two-slope log-log fit (disable with --no-aperiodic-two-slope)\n"
      << "  - aperiodic_slope_low  : low-frequency slope of the two-slope log-log fit (log10 domain; negative for 1/f^k)\n"
      << "  - aperiodic_slope_high : high-frequency slope of the two-slope log-log fit (log10 domain; negative for 1/f^k)\n"
      << "  - aperiodic_exponent_low  : low-frequency exponent k (=-aperiodic_slope_low)\n"
      << "  - aperiodic_exponent_high : high-frequency exponent k (=-aperiodic_slope_high)\n"
      << "  - aperiodic_r2_two_slope  : R^2 goodness of the two-slope log-log fit (log10 domain)\n"
      << "  - aperiodic_rmse_two_slope: RMSE of the two-slope log-log fit in log10(PSD) units\n"
      << "  - aperiodic_aic_two_slope : AIC for the two-slope log-log aperiodic fit (unweighted residuals)\n"
      << "  - aperiodic_bic_two_slope : BIC for the two-slope log-log aperiodic fit (unweighted residuals)\n"
      << "  - aperiodic_best_model_aic: model name with the lowest AIC among enabled aperiodic models\n"
      << "  - aperiodic_best_model_bic: model name with the lowest BIC among enabled aperiodic models\n"
      << "  - aperiodic_delta_aic_{loglog,two_slope,knee}: ΔAIC for each model relative to the best (lowest) AIC (0=best)\n"
      << "  - aperiodic_aic_weight_{loglog,two_slope,knee}: normalized Akaike weights from ΔAIC (sum to 1 across enabled finite models)\n"
      << "  - aperiodic_delta_bic_{loglog,two_slope,knee}: ΔBIC for each model relative to the best (lowest) BIC (0=best)\n"
      << "  - aperiodic_bic_weight_{loglog,two_slope,knee}: normalized weights from ΔBIC (sum to 1 across enabled finite models)\n"
      << "  - aperiodic_background_used: aperiodic background model actually used for prominence/periodic residual metrics (after fallbacks)\n"
      << "  - periodic_power     : integrated power above the fitted aperiodic background within [fmin,fmax]\n"
      << "  - periodic_rel       : periodic_power / total_power within [fmin,fmax]\n\n"
      << "  - {band}_power : bandpower integrated over a standard EEG band (delta/theta/alpha/beta/gamma)\n"
      << "  - {band}_rel   : relative bandpower ({band}_power / total_power)\n"
      << "  - {band}_periodic_power : periodic bandpower above aperiodic background within that band\n"
      << "  - {band}_periodic_rel   : {band}_periodic_power / total_power\n"
      << "  - {band}_periodic_frac  : {band}_periodic_power / periodic_power\n"
      << "  - {band}_prominent_peak_hz : frequency of most prominent oscillatory peak within the band (max prominence vs aperiodic fit)\n"
      << "  - {band}_prominent_peak_hz_refined : prominent peak refined by quadratic interpolation on the prominence curve\n"
      << "  - {band}_prominent_peak_value_db   : PSD value at {band}_prominent_peak_hz expressed in dB (10*log10)\n"
      << "  - {band}_prominent_peak_fwhm_hz    : FWHM around {band}_prominent_peak_hz within the band\n"
      << "  - {band}_prominent_peak_q          : Q factor = {band}_prominent_peak_hz / {band}_prominent_peak_fwhm_hz\n"
      << "  - {band}_prominent_peak_prominence_db : prominence (dB) at that peak relative to aperiodic fit\n"
      << "  - theta_beta   : (theta_power) / (beta_power)\n"
      << "  - alpha_theta  : (alpha_power) / (theta_power)\n\n"
      << "Usage:\n"
      << "  qeeg_spectral_features_cli --input file.edf --outdir out_spec\n"
      << "  qeeg_spectral_features_cli --input file.csv --fs 250 --outdir out_spec\n"
      << "  qeeg_spectral_features_cli --input file.edf --outdir out_spec --range 1 40 --edge 0.95\n\n"
      << "Options:\n"
      << "  --input PATH            Input EDF/BDF/CSV/ASCII/BrainVision (.vhdr)\n"
      << "  --fs HZ                 Sampling rate hint for CSV (0 = infer from time column)\n"
      << "  --outdir DIR            Output directory (default: out_spectral)\n"
      << "  --nperseg N             Welch segment length (default: 1024)\n"
      << "  --overlap FRAC          Welch overlap fraction in [0,1) (default: 0.5)\n"
      << "  --range LO HI           Frequency range in Hz (default: 1 40)\n"
      << "  --aperiodic-range LO HI Fit range for the aperiodic (log-log) model (default: same as --range)\n"
      << "  --aperiodic-exclude LO HI Exclude a frequency interval from the aperiodic fit (repeatable).\n"
      << "  --no-aperiodic-knee-model Disable optional curved aperiodic knee model columns (offset_knee, knee_param, etc).\n"
      << "  --no-aperiodic-two-slope Disable the optional two-slope aperiodic fit columns (knee + low/high slopes).\n"
      << "  --aperiodic-two-slope-min-points N Minimum points per side when estimating the knee (default: 6).\n"
      << "  --aperiodic-background MODEL Select aperiodic background model for prominence/periodic residual metrics:\n"
      << "                          loglog | two_slope | knee | auto_aic | auto_bic (default: loglog).\n"
      << "  --alpha-range LO HI     Alpha peak search range in Hz (default: 8 12)\n"
      << "  --no-bands              Do not output bandpower/relative-bandpower columns\n"
      << "  --no-band-peaks         Do not output {band}_prominent_peak_* columns\n"
      << "  --no-periodic-bands     Do not output {band}_periodic_* columns\n"
      << "  --band NAME LO HI        Add a custom band (repeatable). First --band clears defaults.\n"
      << "  --no-ratios             Do not output ratio columns (theta_beta, alpha_theta by default)\n"
      << "  --ratio COL NUM DEN     Add a custom ratio column COL = NUM/ DEN (repeatable). First --ratio clears defaults.\n"
      << "  --edge X                Spectral edge fraction in (0,1]; repeatable (default: 0.95).\n"
      << "                          The first --edge clears the default list.\n"
      << "  --average-reference     Apply common average reference across channels\n"
      << "  --notch HZ              Apply a notch filter at HZ (e.g., 50 or 60)\n"
      << "  --notch-q Q             Notch Q factor (default: 30)\n"
      << "  --bandpass LO HI        Apply a simple bandpass (highpass LO then lowpass HI)\n"
      << "  --zero-phase            Offline: forward-backward filtering (less phase distortion)\n"
      << "  -h, --help              Show this help\n";
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--input" && i + 1 < argc) {
      a.input_path = argv[++i];
    } else if (arg == "--outdir" && i + 1 < argc) {
      a.outdir = argv[++i];
    } else if (arg == "--fs" && i + 1 < argc) {
      a.fs_csv = to_double(argv[++i]);
    } else if (arg == "--nperseg" && i + 1 < argc) {
      a.nperseg = static_cast<size_t>(to_int(argv[++i]));
    } else if (arg == "--overlap" && i + 1 < argc) {
      a.overlap = to_double(argv[++i]);
    } else if ((arg == "--range" || arg == "--freq-range") && i + 2 < argc) {
      a.fmin_hz = to_double(argv[++i]);
      a.fmax_hz = to_double(argv[++i]);
    } else if (arg == "--alpha-range" && i + 2 < argc) {
      a.alpha_min_hz = to_double(argv[++i]);
      a.alpha_max_hz = to_double(argv[++i]);
    } else if (arg == "--aperiodic-range" && i + 2 < argc) {
      a.aperiodic_range_set = true;
      a.aperiodic_min_hz = to_double(argv[++i]);
      a.aperiodic_max_hz = to_double(argv[++i]);
    } else if (arg == "--aperiodic-exclude" && i + 2 < argc) {
      FrequencyRange r;
      r.fmin_hz = to_double(argv[++i]);
      r.fmax_hz = to_double(argv[++i]);
      a.aperiodic_excludes.push_back(r);
    } else if (arg == "--no-aperiodic-knee-model") {
      a.include_aperiodic_knee_model = false;
    } else if (arg == "--no-aperiodic-two-slope") {
      a.include_aperiodic_two_slope = false;
    } else if (arg == "--aperiodic-two-slope-min-points" && i + 1 < argc) {
      a.aperiodic_two_slope_min_points_per_side = static_cast<std::size_t>(to_int(argv[++i]));
    } else if (arg == "--aperiodic-background" && i + 1 < argc) {
      a.aperiodic_background = parse_aperiodic_background_mode(argv[++i]);
    } else if (arg == "--no-bands") {
      a.include_bands = false;
    } else if (arg == "--no-band-peaks") {
      a.include_band_peaks = false;
    } else if (arg == "--no-periodic-bands") {
      a.include_periodic_bands = false;
    } else if (arg == "--band" && i + 3 < argc) {
      if (!a.bands_custom) {
        a.bands_custom = true;
        a.bands.clear();
      }
      Args::BandDef b;
      b.name = argv[++i];
      b.key = sanitize_key(b.name);
      b.col_power = b.key + "_power";
      b.col_rel = b.key + "_rel";
      b.col_periodic_power = b.key + "_periodic_power";
      b.col_periodic_rel = b.key + "_periodic_rel";
      b.col_periodic_frac = b.key + "_periodic_frac";
      b.col_prominent_peak_hz = b.key + "_prominent_peak_hz";
      b.col_prominent_peak_hz_refined = b.key + "_prominent_peak_hz_refined";
      b.col_prominent_peak_value_db = b.key + "_prominent_peak_value_db";
      b.col_prominent_peak_fwhm_hz = b.key + "_prominent_peak_fwhm_hz";
      b.col_prominent_peak_q = b.key + "_prominent_peak_q";
      b.col_prominent_peak_prominence_db = b.key + "_prominent_peak_prominence_db";
      b.lo_hz = to_double(argv[++i]);
      b.hi_hz = to_double(argv[++i]);
      a.bands.push_back(b);
    } else if (arg == "--no-ratios") {
      a.include_ratios = false;
    } else if (arg == "--ratio" && i + 3 < argc) {
      if (!a.ratios_custom) {
        a.ratios_custom = true;
        a.ratios.clear();
      }
      Args::RatioDef r;
      r.col = sanitize_key(argv[++i]);
      r.num_key = sanitize_key(argv[++i]);
      r.den_key = sanitize_key(argv[++i]);
      a.ratios.push_back(r);
    } else if (arg == "--edge" && i + 1 < argc) {
      if (!a.edges_custom) {
        a.edges_custom = true;
        a.edges.clear();
      }
      a.edges.push_back(to_double(argv[++i]));
    } else if (arg == "--average-reference") {
      a.average_reference = true;
    } else if (arg == "--notch" && i + 1 < argc) {
      a.notch_hz = to_double(argv[++i]);
    } else if (arg == "--notch-q" && i + 1 < argc) {
      a.notch_q = to_double(argv[++i]);
    } else if (arg == "--bandpass" && i + 2 < argc) {
      a.bandpass_low_hz = to_double(argv[++i]);
      a.bandpass_high_hz = to_double(argv[++i]);
    } else if (arg == "--zero-phase") {
      a.zero_phase = true;
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + arg);
    }
  }
  if (!a.aperiodic_range_set) {
    a.aperiodic_min_hz = a.fmin_hz;
    a.aperiodic_max_hz = a.fmax_hz;
  }

  return a;
}

static std::string edge_col_name(double edge, const std::string& prefix = std::string()) {
  // e.g. 0.95 -> sef95_hz
  const int pct = static_cast<int>(std::llround(edge * 100.0));
  const std::string base = "sef" + std::to_string(pct) + "_hz";
  return prefix.empty() ? base : (prefix + base);
}

static void write_sidecar_json(const Args& args) {
  const std::string outpath = args.outdir + "/spectral_features.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write spectral_features.json: " + outpath);
  out << std::setprecision(12);

  const std::vector<double> edges_used = args.edges.empty() ? std::vector<double>{0.95} : args.edges;

  struct Entry {
    std::string key;
    std::string long_name;
    std::string desc;
    std::string units;
  };

  std::vector<Entry> entries;
  entries.reserve(96);

  auto add = [&](const std::string& key,
                 const std::string& long_name,
                 const std::string& desc,
                 const std::string& units) {
    entries.push_back(Entry{key, long_name, desc, units});
  };

  const std::string range = "[" + fmt_double(args.fmin_hz, 4) + "," + fmt_double(args.fmax_hz, 4) + "] Hz";

  const std::string alpha_range = "[" + fmt_double(args.alpha_min_hz, 4) + "," + fmt_double(args.alpha_max_hz, 4) + "] Hz";

  const std::string ap_range = "[" + fmt_double(args.aperiodic_min_hz, 4) + "," + fmt_double(args.aperiodic_max_hz, 4) + "] Hz";

  add("channel", "Channel label", "EEG channel label (one row per channel).", "");
  add("total_power", "Total power", "Total power (integral of PSD) within " + range + ".", "a.u.");
  add("entropy",
      "Spectral entropy (normalized)",
      "Normalized spectral entropy within " + range + ". Values are in [0,1] (higher means flatter spectrum).",
      "n/a");
  add("mean_hz", "Mean frequency (spectral centroid)", "Power-weighted mean frequency within " + range + ".", "Hz");
  add("bandwidth_hz",
      "Spectral bandwidth",
      "Power-weighted standard deviation of frequency within " + range + ".",
      "Hz");
  add("skewness",
      "Spectral skewness",
      "Skewness of the power-weighted frequency distribution within " + range + " (dimensionless).",
      "n/a");
  add("kurtosis_excess",
      "Spectral excess kurtosis",
      "Excess kurtosis (kurtosis-3) of the power-weighted frequency distribution within " + range + " (dimensionless).",
      "n/a");
  add("flatness",
      "Spectral flatness",
      "Spectral flatness within " + range + " (geometric_mean/arith_mean of PSD; values in [0,1]).",
      "n/a");
  add("peak_hz",
      "Peak frequency",
      "Frequency of maximum PSD within " + range + " (simple argmax; includes exact range boundaries).",
      "Hz");
  add("peak_hz_refined",
      "Peak frequency (refined)",
      "Peak frequency refined by quadratic (parabolic) interpolation around peak_hz within " + range + ".",
      "Hz");
  add("peak_value_db",
      "Peak PSD value (dB)",
      "PSD value at peak_hz expressed in dB (10*log10) within " + range + ".",
      "dB");
  add("peak_fwhm_hz",
      "Peak bandwidth (FWHM)",
      "Full-width at half-maximum (FWHM) around peak_hz within " + range + ".",
      "Hz");
  add("peak_q",
      "Peak Q factor",
      "Q factor computed as peak_hz / peak_fwhm_hz within " + range + ".",
      "n/a");
  add("peak_prominence_db",
      "Peak prominence (dB)",
      "Peak prominence in dB at peak_hz relative to the selected aperiodic background model within " + range + ".",
      "dB");
  add("prominent_peak_hz",
      "Most prominent peak frequency",
      "Frequency of the most prominent oscillatory peak (maximum prominence above the selected aperiodic background model) within " + range + ".",
      "Hz");
  add("prominent_peak_hz_refined",
      "Most prominent peak frequency (refined)",
      "Most prominent peak frequency refined by quadratic (parabolic) interpolation around prominent_peak_hz within " + range + ".",
      "Hz");
  add("prominent_peak_value_db",
      "Most prominent peak PSD value (dB)",
      "PSD value at prominent_peak_hz expressed in dB (10*log10) within " + range + ".",
      "dB");
  add("prominent_peak_fwhm_hz",
      "Most prominent peak bandwidth (FWHM)",
      "Full-width at half-maximum (FWHM) around prominent_peak_hz within " + range + ".",
      "Hz");
  add("prominent_peak_q",
      "Most prominent peak Q factor",
      "Q factor computed as prominent_peak_hz / prominent_peak_fwhm_hz within " + range + ".",
      "n/a");
  add("prominent_peak_prominence_db",
      "Most prominent peak prominence (dB)",
      "Maximum peak prominence in dB relative to the selected aperiodic background model within " + range + ".",
      "dB");
  add("alpha_peak_hz",
      "Alpha peak frequency",
      "Peak frequency within alpha range " + alpha_range + " (intersected with the analysis range).",
      "Hz");
  add("alpha_peak_hz_refined",
      "Alpha peak frequency (refined)",
      "Alpha peak frequency refined by quadratic (parabolic) interpolation around alpha_peak_hz within alpha range " +
          alpha_range + " (intersected with the analysis range).",
      "Hz");
  add("alpha_peak_value_db",
      "Alpha PSD value (dB)",
      "PSD value at alpha_peak_hz expressed in dB (10*log10) within alpha range " + alpha_range + ".",
      "dB");
  add("alpha_fwhm_hz",
      "Alpha peak bandwidth (FWHM)",
      "Full-width at half-maximum (FWHM) around alpha_peak_hz within alpha range " + alpha_range + ".",
      "Hz");
  add("alpha_q",
      "Alpha peak Q factor",
      "Q factor computed as alpha_peak_hz / alpha_fwhm_hz within alpha range " + alpha_range + ".",
      "n/a");
  add("alpha_prominence_db",
      "Alpha peak prominence (dB)",
      "Alpha peak prominence in dB at alpha_peak_hz relative to the selected aperiodic background model within " + range + ".",
      "dB");
  add("median_hz", "Median frequency (SEF50)", "Spectral edge frequency at 50% cumulative power within " + range + ".", "Hz");

  // Raw spectral edge frequencies (one per --edge).
  for (double e : edges_used) {
    const int pct = static_cast<int>(std::llround(e * 100.0));
    const std::string col = edge_col_name(e);
    add(col,
        "Spectral edge frequency (SEF" + std::to_string(pct) + ")",
        "Spectral edge frequency at " + std::to_string(pct) + "% cumulative power within " + range + ".",
        "Hz");
  }

  std::string ap_excl;
  for (const auto& r : args.aperiodic_excludes) {
    if (!(r.fmax_hz > r.fmin_hz) || !std::isfinite(r.fmin_hz) || !std::isfinite(r.fmax_hz)) continue;
    if (!ap_excl.empty()) ap_excl += ", ";
    ap_excl += "[" + fmt_double(r.fmin_hz, 4) + "," + fmt_double(r.fmax_hz, 4) + "] Hz";
  }
  if (!ap_excl.empty()) ap_excl = "; excluding " + ap_excl;

  const std::string ap_note = ap_range + " (intersected with analysis range " + range + ")" + ap_excl;

  add("aperiodic_offset",
      "Aperiodic offset (log-log intercept)",
      "Intercept of a robust linear fit of log10(PSD) vs log10(frequency) within " + ap_note + ". "
      "This approximates log10(power) at 1 Hz for a 1/f^k model.",
      "log10(a.u.)");
  add("aperiodic_slope",
      "Aperiodic slope (log-log)",
      "Slope of a robust linear fit of log10(PSD) vs log10(frequency) within " + ap_note + ". "
      "For PSD ≈ A / f^k, this slope is approximately -k.",
      "n/a");
  add("aperiodic_exponent",
      "Aperiodic exponent (1/f^k)",
      "Exponent k from a robust 1/f^k fit within " + ap_note + " (computed as -slope in log-log space).",
      "n/a");
  add("aperiodic_r2",
      "Aperiodic fit R^2",
      "R^2 goodness-of-fit for the robust log-log linear fit within " + ap_note + ".",
      "n/a");
  add("aperiodic_rmse",
      "Aperiodic fit RMSE",
      "Root-mean-square error of the robust log-log linear fit within " + ap_note + " (in log10(PSD) units).",
      "log10(a.u.)");
  add("aperiodic_n_points",
      "Aperiodic fit N points",
      "Number of sample points used in the aperiodic (log-log) fit within " + ap_note + ".",
      "count");
  add("aperiodic_offset_db",
      "Aperiodic offset (dB)",
      "Aperiodic offset in dB: 10*aperiodic_offset. This is approximately the predicted 10*log10(PSD) at 1 Hz for the fitted 1/f^k model.",
      "dB");

  add("aperiodic_aic",
      "Aperiodic model AIC (log-log)",
      "Akaike Information Criterion (AIC) for the single-slope log-log aperiodic fit within " + ap_note + ". Computed from the *unweighted* log10(PSD) residuals (weights=1) and a 2-parameter linear model.",
      "n/a");
  add("aperiodic_bic",
      "Aperiodic model BIC (log-log)",
      "Bayesian Information Criterion (BIC) for the single-slope log-log aperiodic fit within " + ap_note + ". Computed from the *unweighted* log10(PSD) residuals (weights=1) and a 2-parameter linear model.",
      "n/a");

  if (args.include_aperiodic_two_slope) {
    add("aperiodic_knee_hz",
        "Aperiodic knee frequency (two-slope)",
        "Estimated knee frequency for a continuous two-slope fit of log10(PSD) vs log10(frequency) within " + ap_note + ". "
        "The knee is selected by scanning candidate breakpoints and minimizing OLS error, then optionally refined with Huber IRLS.",
        "Hz");
    add("aperiodic_slope_low",
        "Aperiodic slope (low frequencies, two-slope)",
        "Low-frequency slope of the two-slope log-log aperiodic fit within " + ap_note + ". For PSD ≈ A / f^k, slope ≈ -k.",
        "n/a");
    add("aperiodic_slope_high",
        "Aperiodic slope (high frequencies, two-slope)",
        "High-frequency slope of the two-slope log-log aperiodic fit within " + ap_note + ". For PSD ≈ A / f^k, slope ≈ -k.",
        "n/a");
    add("aperiodic_exponent_low",
        "Aperiodic exponent (low frequencies, two-slope)",
        "Low-frequency exponent k from the two-slope fit within " + ap_note + " (computed as -aperiodic_slope_low).",
        "n/a");
    add("aperiodic_exponent_high",
        "Aperiodic exponent (high frequencies, two-slope)",
        "High-frequency exponent k from the two-slope fit within " + ap_note + " (computed as -aperiodic_slope_high).",
        "n/a");
    add("aperiodic_r2_two_slope",
        "Aperiodic fit R^2 (two-slope)",
        "R^2 goodness-of-fit for the two-slope aperiodic fit within " + ap_note + " (log10 domain).",
        "n/a");
    add("aperiodic_rmse_two_slope",
        "Aperiodic fit RMSE (two-slope)",
        "Root-mean-square error of the two-slope aperiodic fit within " + ap_note + " (in log10(PSD) units).",
        "log10(a.u.)");
    add("aperiodic_aic_two_slope",
        "Aperiodic model AIC (two-slope)",
        "Akaike Information Criterion (AIC) for the continuous two-slope log-log aperiodic fit within " + ap_note + ". Computed from the *unweighted* log10(PSD) residuals (weights=1) and a 4-parameter model (knee + two slopes + continuous offset).",
        "n/a");
    add("aperiodic_bic_two_slope",
        "Aperiodic model BIC (two-slope)",
        "Bayesian Information Criterion (BIC) for the continuous two-slope log-log aperiodic fit within " + ap_note + ". Computed from the *unweighted* log10(PSD) residuals (weights=1) and a 4-parameter model (knee + two slopes + continuous offset).",
        "n/a");
  }

  if (args.include_aperiodic_knee_model) {
    add("aperiodic_offset_knee",
        "Aperiodic offset (knee model)",
        "Offset of a curved aperiodic knee model fitted within " + ap_note + ". "
        "Model: log10(PSD(f)) = offset - log10(knee + f^exponent).",
        "log10(a.u.)");
    add("aperiodic_exponent_knee",
        "Aperiodic exponent (knee model)",
        "Exponent parameter of the curved knee model fitted within " + ap_note + ". "
        "When knee=0 this matches the 1/f^k exponent.",
        "n/a");
    add("aperiodic_knee_param",
        "Aperiodic knee parameter (knee model)",
        "Knee parameter of the curved knee model within " + ap_note + ". "
        "Note: knee has units of Hz^exponent in the model.",
        "Hz^exponent");
    add("aperiodic_knee_freq_hz",
        "Aperiodic knee frequency (knee model)",
        "Approximate knee frequency derived from the knee parameter within " + ap_note + ": knee_freq_hz = knee^(1/exponent).",
        "Hz");
    add("aperiodic_r2_knee",
        "Aperiodic fit R^2 (knee model)",
        "R^2 goodness-of-fit for the curved knee model within " + ap_note + " (log10 domain).",
        "n/a");
    add("aperiodic_rmse_knee",
        "Aperiodic fit RMSE (knee model)",
        "Root-mean-square error of the curved knee model within " + ap_note + " (in log10(PSD) units).",
        "log10(a.u.)");
    add("aperiodic_n_points_knee",
        "Aperiodic fit N points (knee model)",
        "Number of sample points used in the curved knee model fit within " + ap_note + ".",
        "count");

    add("aperiodic_aic_knee",
        "Aperiodic model AIC (knee model)",
        "Akaike Information Criterion (AIC) for the curved aperiodic knee model within " + ap_note + ". Computed from the *unweighted* log10(PSD) residuals (weights=1) and a 3-parameter model (offset, exponent, knee).",
        "n/a");
    add("aperiodic_bic_knee",
        "Aperiodic model BIC (knee model)",
        "Bayesian Information Criterion (BIC) for the curved aperiodic knee model within " + ap_note + ". Computed from the *unweighted* log10(PSD) residuals (weights=1) and a 3-parameter model (offset, exponent, knee).",
        "n/a");
  }

  add("aperiodic_best_model_aic",
      "Best aperiodic model (AIC)",
      "Name of the aperiodic model with the lowest AIC among the enabled candidates (loglog, two_slope, knee). AIC is computed from the *unweighted* log10(PSD) residuals (weights=1).",
      "n/a");
  add("aperiodic_best_model_bic",
      "Best aperiodic model (BIC)",
      "Name of the aperiodic model with the lowest BIC among the enabled candidates (loglog, two_slope, knee). BIC is computed from the *unweighted* log10(PSD) residuals (weights=1).",
      "n/a");

  // Model comparison diagnostics (Δ and weights) from information criteria.
  // Weights are computed as exp(-0.5*Δ) normalized across the enabled models with finite scores.
  add("aperiodic_delta_aic_loglog",
      "Aperiodic ΔAIC (log-log)",
      "Delta AIC for the single-slope log-log aperiodic model within " + ap_note + ". ΔAIC is computed relative to the lowest AIC among the enabled candidates (0 = best).",
      "n/a");
  add("aperiodic_aic_weight_loglog",
      "Aperiodic Akaike weight (log-log)",
      "Normalized Akaike weight for the single-slope log-log aperiodic model within " + ap_note + ". Computed from ΔAIC as exp(-0.5*Δ) and normalized across enabled candidates.",
      "n/a");
  if (args.include_aperiodic_two_slope) {
    add("aperiodic_delta_aic_two_slope",
        "Aperiodic ΔAIC (two-slope)",
        "Delta AIC for the two-slope aperiodic model within " + ap_note + ". ΔAIC is computed relative to the lowest AIC among the enabled candidates (0 = best).",
        "n/a");
    add("aperiodic_aic_weight_two_slope",
        "Aperiodic Akaike weight (two-slope)",
        "Normalized Akaike weight for the two-slope aperiodic model within " + ap_note + ". Computed from ΔAIC as exp(-0.5*Δ) and normalized across enabled candidates.",
        "n/a");
  }
  if (args.include_aperiodic_knee_model) {
    add("aperiodic_delta_aic_knee",
        "Aperiodic ΔAIC (knee model)",
        "Delta AIC for the curved knee aperiodic model within " + ap_note + ". ΔAIC is computed relative to the lowest AIC among the enabled candidates (0 = best).",
        "n/a");
    add("aperiodic_aic_weight_knee",
        "Aperiodic Akaike weight (knee model)",
        "Normalized Akaike weight for the curved knee aperiodic model within " + ap_note + ". Computed from ΔAIC as exp(-0.5*Δ) and normalized across enabled candidates.",
        "n/a");
  }

  add("aperiodic_delta_bic_loglog",
      "Aperiodic ΔBIC (log-log)",
      "Delta BIC for the single-slope log-log aperiodic model within " + ap_note + ". ΔBIC is computed relative to the lowest BIC among the enabled candidates (0 = best).",
      "n/a");
  add("aperiodic_bic_weight_loglog",
      "Aperiodic BIC weight (log-log)",
      "Normalized weight for the single-slope log-log aperiodic model within " + ap_note + ". Computed from ΔBIC as exp(-0.5*Δ) and normalized across enabled candidates.",
      "n/a");
  if (args.include_aperiodic_two_slope) {
    add("aperiodic_delta_bic_two_slope",
        "Aperiodic ΔBIC (two-slope)",
        "Delta BIC for the two-slope aperiodic model within " + ap_note + ". ΔBIC is computed relative to the lowest BIC among the enabled candidates (0 = best).",
        "n/a");
    add("aperiodic_bic_weight_two_slope",
        "Aperiodic BIC weight (two-slope)",
        "Normalized weight for the two-slope aperiodic model within " + ap_note + ". Computed from ΔBIC as exp(-0.5*Δ) and normalized across enabled candidates.",
        "n/a");
  }
  if (args.include_aperiodic_knee_model) {
    add("aperiodic_delta_bic_knee",
        "Aperiodic ΔBIC (knee model)",
        "Delta BIC for the curved knee aperiodic model within " + ap_note + ". ΔBIC is computed relative to the lowest BIC among the enabled candidates (0 = best).",
        "n/a");
    add("aperiodic_bic_weight_knee",
        "Aperiodic BIC weight (knee model)",
        "Normalized weight for the curved knee aperiodic model within " + ap_note + ". Computed from ΔBIC as exp(-0.5*Δ) and normalized across enabled candidates.",
        "n/a");
  }

  add("aperiodic_background_used",
      "Aperiodic background model used",
      "Aperiodic background model actually used for peak prominence and periodic residual metrics. This is the per-channel result of --aperiodic-background (including auto selection) with fallbacks to loglog when the requested model is unavailable.",
      "n/a");

  add("periodic_power",
      "Periodic power above aperiodic",
      "Integrated power above the fitted aperiodic background within " + range + ". "
      "Computed as ∫ max(0, PSD(f) - PSD_aperiodic(f)) df using the selected aperiodic background model computed within " + ap_note + ".",
      "a.u.");
  add("periodic_rel",
      "Periodic power fraction",
      "Periodic power fraction within " + range + ": (periodic_power) / (total_power).",
      "n/a");

  // Periodic spectral edge frequencies on the aperiodic-adjusted residual.
  add("periodic_median_hz",
      "Periodic median frequency (periodic SEF50)",
      "Spectral edge frequency at 50% cumulative periodic power within " + range + ". "
      "Periodic power is defined as max(0, PSD(f) - PSD_aperiodic(f)) using the selected aperiodic background model computed within " + ap_note + ".",
      "Hz");
  for (double e : edges_used) {
    const int pct = static_cast<int>(std::llround(e * 100.0));
    const std::string col = edge_col_name(e, "periodic_");
    add(col,
        "Periodic spectral edge frequency (periodic SEF" + std::to_string(pct) + ")",
        "Spectral edge frequency at " + std::to_string(pct) + "% cumulative periodic power within " + range + ". "
        "Periodic power is defined as max(0, PSD(f) - PSD_aperiodic(f)) using the selected aperiodic background model computed within " + ap_note + ".",
        "Hz");
  }

  const std::vector<Args::BandDef> bands_used = args.include_bands
                                                    ? (args.bands.empty() ? default_bands() : args.bands)
                                                    : std::vector<Args::BandDef>{};
  const std::vector<Args::RatioDef> ratios_used = (args.include_ratios && !bands_used.empty())
                                                      ? (args.ratios.empty() ? default_ratios(bands_used) : args.ratios)
                                                      : std::vector<Args::RatioDef>{};

  if (!bands_used.empty()) {
    const std::string note = " (intersected with the analysis range)";
    for (const auto& b : bands_used) {
      const std::string band_range = "[" + fmt_double(b.lo_hz, 4) + "," + fmt_double(b.hi_hz, 4) + "] Hz";
      add(b.col_power,
          b.name + " band power",
          b.name + " bandpower integrated over " + band_range + note + ".",
          "a.u.");
      add(b.col_rel,
          b.name + " relative band power",
          "Relative " + b.name + " bandpower: (" + b.col_power + ") / (total_power) within " + range + ".",
          "n/a");
      if (args.include_periodic_bands) {
        add(b.col_periodic_power,
            b.name + " periodic band power",
            "Periodic power above the fitted aperiodic background integrated over " + band_range + note + ". "
            "Computed as ∫ max(0, PSD(f) - PSD_aperiodic(f)) df using the selected aperiodic background model.",
            "a.u.");
        add(b.col_periodic_rel,
            b.name + " periodic relative band power",
            "Relative periodic " + b.name + " bandpower: (" + b.col_periodic_power + ") / (total_power) within " + range + ".",
            "n/a");
        add(b.col_periodic_frac,
            b.name + " periodic band fraction",
            "Fraction of periodic power in " + b.name + " band: (" + b.col_periodic_power + ") / (periodic_power) within " + range + ".",
            "n/a");
      }

      if (args.include_band_peaks) {
        add(b.col_prominent_peak_hz,
            b.name + " prominent peak frequency",
            "Frequency of the most prominent oscillatory peak within " + band_range + note + ". "
            "The peak is selected as the maximum positive prominence (in dB) above the selected aperiodic background model computed within " + ap_note + ".",
            "Hz");
        add(b.col_prominent_peak_hz_refined,
            b.name + " prominent peak frequency (refined)",
            "Prominent peak frequency refined by quadratic (parabolic) interpolation on the prominence curve within " + band_range + note + ".",
            "Hz");
        add(b.col_prominent_peak_value_db,
            b.name + " prominent peak PSD value (dB)",
            "PSD value at " + b.col_prominent_peak_hz + " expressed in dB (10*log10) within " + band_range + note + ".",
            "dB");
        add(b.col_prominent_peak_fwhm_hz,
            b.name + " prominent peak bandwidth (FWHM)",
            "Full-width at half-maximum (FWHM) around " + b.col_prominent_peak_hz + " within " + band_range + note + ".",
            "Hz");
        add(b.col_prominent_peak_q,
            b.name + " prominent peak Q factor",
            "Q factor computed as (" + b.col_prominent_peak_hz + ") / (" + b.col_prominent_peak_fwhm_hz + ") within " + band_range + note + ".",
            "n/a");
        add(b.col_prominent_peak_prominence_db,
            b.name + " prominent peak prominence (dB)",
            "Prominence (dB) at " + b.col_prominent_peak_hz + " relative to the selected aperiodic background model computed within " + ap_note + ".",
            "dB");
      }
    }
  }

  if (!ratios_used.empty()) {
    for (const auto& r : ratios_used) {
      add(r.col,
          r.col + " band ratio",
          "Ratio computed as (" + r.num_key + "_power) / (" + r.den_key + "_power).",
          "n/a");
    }
  }

  out << "{\n";
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    out << "  \"" << json_escape(e.key) << "\": {\n";
    out << "    \"LongName\": \"" << json_escape(e.long_name) << "\",\n";
    out << "    \"Description\": \"" << json_escape(e.desc) << "\"";
    if (!e.units.empty()) {
      out << ",\n    \"Units\": \"" << json_escape(e.units) << "\"";
    }
    out << "\n  }";
    if (i + 1 < entries.size()) out << ",";
    out << "\n";
  }
  out << "}\n";
}



static void write_params_json(const Args& args,
                             const EEGRecording& rec,
                             const PreprocessOptions& popt,
                             const WelchOptions& wopt,
                             double fmin_used,
                             double fmax_used,
                             double a_fmin_used,
                             double a_fmax_used,
                             const std::vector<FrequencyRange>& aperiodic_excludes_used,
                             const std::vector<double>& edges_used,
                             const std::vector<Args::BandDef>& bands,
                             const std::vector<Args::RatioDef>& ratios,
                             bool include_periodic_bands,
                             bool include_band_peaks) {
  const std::string outpath = args.outdir + "/spectral_features_params.json";
  std::ofstream out(std::filesystem::u8path(outpath), std::ios::binary);
  if (!out) throw std::runtime_error("Failed to write spectral_features_params.json: " + outpath);
  out << std::setprecision(12);

  auto write_bool = [&](bool v) { out << (v ? "true" : "false"); };
  auto write_num = [&](double v) {
    if (!std::isfinite(v)) {
      out << "null";
    } else {
      out << v;
    }
  };

  out << "{\n";
  out << "  \"Tool\": \"qeeg_spectral_features_cli\",\n";
  out << "  \"TimestampUTC\": \"" << json_escape(now_string_utc()) << "\",\n";
  out << "  \"input_path\": \"" << json_escape(args.input_path) << "\",\n";
  out << "  \"output_dir\": \"" << json_escape(args.outdir) << "\",\n";
  out << "  \"fs_hz\": ";
  write_num(rec.fs_hz);
  out << ",\n";
  out << "  \"n_channels\": " << rec.n_channels() << ",\n";
  out << "  \"n_samples\": " << rec.n_samples() << ",\n";

  out << "  \"welch\": {\n";
  out << "    \"nperseg\": " << wopt.nperseg << ",\n";
  out << "    \"overlap_fraction\": ";
  write_num(wopt.overlap_fraction);
  out << "\n  },\n";

  out << "  \"analysis_range_hz\": {\n";
  out << "    \"requested\": [";
  write_num(args.fmin_hz);
  out << ", ";
  write_num(args.fmax_hz);
  out << "],\n";
  out << "    \"used\": [";
  write_num(fmin_used);
  out << ", ";
  write_num(fmax_used);
  out << "]\n  },\n";

  out << "  \"alpha_range_hz\": [";
  write_num(args.alpha_min_hz);
  out << ", ";
  write_num(args.alpha_max_hz);
  out << "],\n";

  out << "  \"aperiodic_fit_range_hz\": {\n";
  out << "    \"requested\": [";
  write_num(args.aperiodic_min_hz);
  out << ", ";
  write_num(args.aperiodic_max_hz);
  out << "],\n";
  out << "    \"used\": [";
  write_num(a_fmin_used);
  out << ", ";
  write_num(a_fmax_used);
  out << "]\n  },\n";

  out << "  \"aperiodic_exclude_ranges_hz\": [";
  for (size_t i = 0; i < aperiodic_excludes_used.size(); ++i) {
    const auto& r = aperiodic_excludes_used[i];
    if (i > 0) out << ", ";
    out << "[";
    write_num(r.fmin_hz);
    out << ", ";
    write_num(r.fmax_hz);
    out << "]";
  }
  out << "],\n";

  out << "  \"aperiodic_two_slope\": {\n";
  out << "    \"enabled\": ";
  write_bool(args.include_aperiodic_two_slope);
  out << ",\n";
  out << "    \"min_points_per_side\": " << args.aperiodic_two_slope_min_points_per_side << "\n";
  out << "  },\n";

  out << "  \"aperiodic_knee_model\": {\n";
  out << "    \"enabled\": ";
  write_bool(args.include_aperiodic_knee_model);
  out << ",\n";
  out << "    \"robust\": true,\n";
  out << "    \"max_iter\": 4\n";
  out << "  },\n";

  out << "  \"aperiodic_background_mode\": \"" << json_escape(aperiodic_background_mode_to_string(args.aperiodic_background)) << "\",\n";

  out << "  \"edges\": [";
  for (size_t i = 0; i < edges_used.size(); ++i) {
    if (i > 0) out << ", ";
    write_num(edges_used[i]);
  }
  out << "],\n";

  out << "  \"outputs\": {\n";
  out << "    \"bands\": ";
  write_bool(args.include_bands);
  out << ",\n";
  out << "    \"ratios\": ";
  write_bool(args.include_ratios);
  out << ",\n";
  out << "    \"periodic_bands\": ";
  write_bool(args.include_periodic_bands);
  out << ",\n";
  out << "    \"band_peaks\": ";
  write_bool(args.include_band_peaks);
  out << ",\n";
  out << "    \"effective_periodic_bands\": ";
  write_bool(include_periodic_bands);
  out << ",\n";
  out << "    \"effective_band_peaks\": ";
  write_bool(include_band_peaks);
  out << "\n  },\n";

  out << "  \"preprocess\": {\n";
  out << "    \"average_reference\": ";
  write_bool(popt.average_reference);
  out << ",\n";
  out << "    \"notch_hz\": ";
  write_num(popt.notch_hz);
  out << ",\n";
  out << "    \"notch_q\": ";
  write_num(popt.notch_q);
  out << ",\n";
  out << "    \"bandpass_low_hz\": ";
  write_num(popt.bandpass_low_hz);
  out << ",\n";
  out << "    \"bandpass_high_hz\": ";
  write_num(popt.bandpass_high_hz);
  out << ",\n";
  out << "    \"zero_phase\": ";
  write_bool(popt.zero_phase);
  out << "\n  },\n";

  out << "  \"bands\": [\n";
  for (size_t i = 0; i < bands.size(); ++i) {
    const auto& b = bands[i];
    out << "    {\"name\": \"" << json_escape(b.name) << "\", \"key\": \"" << json_escape(b.key)
        << "\", \"fmin_hz\": ";
    write_num(b.lo_hz);
    out << ", \"fmax_hz\": ";
    write_num(b.hi_hz);
    out << "}";
    if (i + 1 < bands.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";

  out << "  \"ratios\": [\n";
  for (size_t i = 0; i < ratios.size(); ++i) {
    const auto& r = ratios[i];
    out << "    {\"col\": \"" << json_escape(r.col) << "\", \"numerator\": \""
        << json_escape(r.num_key) << "\", \"denominator\": \"" << json_escape(r.den_key) << "\"}";
    if (i + 1 < ratios.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";

  out << "}\n";
}
} // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.input_path.empty()) {
      print_help();
      throw std::runtime_error("--input is required");
    }
    if (args.overlap < 0.0 || args.overlap >= 1.0) {
      throw std::runtime_error("--overlap must be in [0,1)");
    }
    if (args.nperseg < 16) {
      throw std::runtime_error("--nperseg too small (>=16 recommended)");
    }
    if (args.fmin_hz < 0.0 || !(args.fmax_hz > args.fmin_hz)) {
      throw std::runtime_error("--range must satisfy 0 <= LO < HI");
    }
    if (args.alpha_min_hz < 0.0 || !(args.alpha_max_hz > args.alpha_min_hz)) {
      throw std::runtime_error("--alpha-range must satisfy 0 <= LO < HI");
    }
    if (args.aperiodic_min_hz < 0.0 || !(args.aperiodic_max_hz > args.aperiodic_min_hz)) {
      throw std::runtime_error("--aperiodic-range must satisfy 0 <= LO < HI");
    }
    for (const auto& r : args.aperiodic_excludes) {
      if (r.fmin_hz < 0.0 || !(r.fmax_hz > r.fmin_hz)) {
        throw std::runtime_error("--aperiodic-exclude must satisfy 0 <= LO < HI");
      }
    }
    if (args.aperiodic_two_slope_min_points_per_side < 2) {
      throw std::runtime_error("--aperiodic-two-slope-min-points must be >= 2");
    }
    if (args.aperiodic_background == AperiodicBackgroundMode::TwoSlope && !args.include_aperiodic_two_slope) {
      throw std::runtime_error("--aperiodic-background two_slope requires the two-slope model to be enabled");
    }
    if (args.aperiodic_background == AperiodicBackgroundMode::Knee && !args.include_aperiodic_knee_model) {
      throw std::runtime_error("--aperiodic-background knee requires the knee model to be enabled");
    }
    for (const auto& b : args.bands) {
      if (!(b.hi_hz > b.lo_hz) || b.lo_hz < 0.0) {
        throw std::runtime_error("--band must satisfy 0 <= LO < HI (band: " + b.name + ")");
      }
    }
    if (!args.include_bands && args.ratios_custom) {
      throw std::runtime_error("--ratio requires band outputs (remove --no-bands)");
    }
    const std::vector<double> edges_used = args.edges.empty() ? std::vector<double>{0.95} : args.edges;
    if (edges_used.empty()) {
      throw std::runtime_error("--edge list is empty (internal error)");
    }
    for (double e : edges_used) {
      if (!std::isfinite(e) || !(e > 0.0 && e <= 1.0)) {
        throw std::runtime_error("--edge must be in (0,1]");
      }
    }

    ensure_directory(args.outdir);

    EEGRecording rec = read_recording_auto(args.input_path, args.fs_csv);

    // Preprocess (offline, in-place)
    PreprocessOptions popt;
    popt.average_reference = args.average_reference;
    popt.notch_hz = args.notch_hz;
    popt.notch_q = args.notch_q;
    popt.bandpass_low_hz = args.bandpass_low_hz;
    popt.bandpass_high_hz = args.bandpass_high_hz;
    popt.zero_phase = args.zero_phase;
    preprocess_recording_inplace(rec, popt);

    WelchOptions wopt;
    wopt.nperseg = args.nperseg;
    wopt.overlap_fraction = args.overlap;

    // Clamp analysis range to Nyquist.
    const double nyq = 0.5 * rec.fs_hz;
    const double fmin = std::max(0.0, args.fmin_hz);
    const double fmax = std::min(args.fmax_hz, nyq);
    if (!(fmax > fmin)) {
      throw std::runtime_error("--range is outside the PSD support (check fs / Nyquist)");
    }

    // Clamp aperiodic fit range to the analysis range and Nyquist.
    const double a_fmin = std::max(fmin, args.aperiodic_min_hz);
    const double a_fmax = std::min(fmax, args.aperiodic_max_hz);
    if (!(a_fmax > a_fmin)) {
      throw std::runtime_error("--aperiodic-range is outside the analysis range / PSD support");
    }

    // Clamp aperiodic exclude ranges to the fit range.
    std::vector<FrequencyRange> aperiodic_excludes_used;
    aperiodic_excludes_used.reserve(args.aperiodic_excludes.size());
    for (const auto& r : args.aperiodic_excludes) {
      const double lo = std::max(a_fmin, r.fmin_hz);
      const double hi = std::min(a_fmax, r.fmax_hz);
      if (!(hi > lo)) continue;
      FrequencyRange rr;
      rr.fmin_hz = lo;
      rr.fmax_hz = hi;
      aperiodic_excludes_used.push_back(rr);
    }

    std::vector<std::string> edge_cols;
    std::vector<std::string> periodic_edge_cols;
    edge_cols.reserve(edges_used.size());
    periodic_edge_cols.reserve(edges_used.size());
    for (double e : edges_used) {
      edge_cols.push_back(edge_col_name(e));
      periodic_edge_cols.push_back(edge_col_name(e, "periodic_"));
    }

    const std::vector<Args::BandDef> bands = args.include_bands ? (args.bands.empty() ? default_bands() : args.bands)
                                                                : std::vector<Args::BandDef>{};
    const std::vector<Args::RatioDef> ratios = args.include_ratios
                                                   ? (args.ratios.empty() ? default_ratios(bands) : args.ratios)
                                                   : std::vector<Args::RatioDef>{};

    const bool include_periodic_bands = args.include_periodic_bands && !bands.empty();
    const bool include_band_peaks = args.include_band_peaks && !bands.empty();

    // Validate band keys/columns are unique.
    {
      std::map<std::string, bool> seen;
      for (const auto& b : bands) {
        if (seen[b.key]) {
          throw std::runtime_error("Duplicate band name after sanitization: " + b.key);
        }
        seen[b.key] = true;
      }
      std::map<std::string, bool> seen_cols;
      auto mark = [&](const std::string& c) {
        if (seen_cols[c]) {
          throw std::runtime_error("Duplicate column name: " + c);
        }
        seen_cols[c] = true;
      };

      // Base output columns.
      mark("channel");
      mark("total_power");
      mark("entropy");
      mark("mean_hz");
      mark("bandwidth_hz");
      mark("skewness");
      mark("kurtosis_excess");
      mark("flatness");
      mark("peak_hz");
      mark("peak_hz_refined");
      mark("peak_value_db");
      mark("peak_fwhm_hz");
      mark("peak_q");
      mark("peak_prominence_db");
      mark("prominent_peak_hz");
      mark("prominent_peak_hz_refined");
      mark("prominent_peak_value_db");
      mark("prominent_peak_fwhm_hz");
      mark("prominent_peak_q");
      mark("prominent_peak_prominence_db");
      mark("alpha_peak_hz");
      mark("alpha_peak_hz_refined");
      mark("alpha_peak_value_db");
      mark("alpha_fwhm_hz");
      mark("alpha_q");
      mark("alpha_prominence_db");
      mark("median_hz");
      for (const auto& c : edge_cols) {
        mark(c);
      }
      mark("periodic_median_hz");
      for (const auto& c : periodic_edge_cols) {
        mark(c);
      }
      mark("aperiodic_offset");
      mark("aperiodic_exponent");
      mark("aperiodic_r2");
      mark("aperiodic_rmse");
      mark("aperiodic_n_points");
      mark("aperiodic_slope");
      mark("aperiodic_offset_db");
      mark("aperiodic_aic");
      mark("aperiodic_bic");
      mark("periodic_power");
      mark("periodic_rel");

      if (args.include_aperiodic_two_slope) {
        mark("aperiodic_knee_hz");
        mark("aperiodic_slope_low");
        mark("aperiodic_slope_high");
        mark("aperiodic_exponent_low");
        mark("aperiodic_exponent_high");
        mark("aperiodic_r2_two_slope");
        mark("aperiodic_rmse_two_slope");
        mark("aperiodic_aic_two_slope");
        mark("aperiodic_bic_two_slope");
      }
      if (args.include_aperiodic_knee_model) {
        mark("aperiodic_offset_knee");
        mark("aperiodic_exponent_knee");
        mark("aperiodic_knee_param");
        mark("aperiodic_knee_freq_hz");
        mark("aperiodic_r2_knee");
        mark("aperiodic_rmse_knee");
        mark("aperiodic_n_points_knee");
        mark("aperiodic_aic_knee");
        mark("aperiodic_bic_knee");
      }
      mark("aperiodic_best_model_aic");
      mark("aperiodic_best_model_bic");
      mark("aperiodic_delta_aic_loglog");
      mark("aperiodic_aic_weight_loglog");
      if (args.include_aperiodic_two_slope) {
        mark("aperiodic_delta_aic_two_slope");
        mark("aperiodic_aic_weight_two_slope");
      }
      if (args.include_aperiodic_knee_model) {
        mark("aperiodic_delta_aic_knee");
        mark("aperiodic_aic_weight_knee");
      }
      mark("aperiodic_delta_bic_loglog");
      mark("aperiodic_bic_weight_loglog");
      if (args.include_aperiodic_two_slope) {
        mark("aperiodic_delta_bic_two_slope");
        mark("aperiodic_bic_weight_two_slope");
      }
      if (args.include_aperiodic_knee_model) {
        mark("aperiodic_delta_bic_knee");
        mark("aperiodic_bic_weight_knee");
      }
      mark("aperiodic_background_used");
      for (const auto& b : bands) {
        mark(b.col_power);
        mark(b.col_rel);
        if (include_periodic_bands) {
          mark(b.col_periodic_power);
          mark(b.col_periodic_rel);
          mark(b.col_periodic_frac);
        }
        if (include_band_peaks) {
          mark(b.col_prominent_peak_hz);
          mark(b.col_prominent_peak_hz_refined);
          mark(b.col_prominent_peak_value_db);
          mark(b.col_prominent_peak_fwhm_hz);
          mark(b.col_prominent_peak_q);
          mark(b.col_prominent_peak_prominence_db);
        }
      }
      for (const auto& r : ratios) {
        mark(r.col);
      }
    }

    std::map<std::string, size_t> band_index;
    for (size_t i = 0; i < bands.size(); ++i) {
      band_index[bands[i].key] = i;
    }

    // Validate ratios reference existing bands.
    for (const auto& r : ratios) {
      if (band_index.find(r.num_key) == band_index.end()) {
        throw std::runtime_error("Ratio " + r.col + " references unknown band: " + r.num_key);
      }
      if (band_index.find(r.den_key) == band_index.end()) {
        throw std::runtime_error("Ratio " + r.col + " references unknown band: " + r.den_key);
      }
    }

    // Compute features.
    struct Row {
      std::string ch;
      double total_power{0.0};
      double entropy{0.0};
      double mean_hz{0.0};
      double bandwidth_hz{0.0};
      double skewness{0.0};
      double kurtosis_excess{0.0};
      double flatness{0.0};
      double peak_hz{0.0};
      double peak_hz_refined{std::numeric_limits<double>::quiet_NaN()};
      double peak_value_db{std::numeric_limits<double>::quiet_NaN()};
      double peak_fwhm_hz{std::numeric_limits<double>::quiet_NaN()};
      double peak_q{std::numeric_limits<double>::quiet_NaN()};
      double peak_prominence_db{std::numeric_limits<double>::quiet_NaN()};
      double prominent_peak_hz{std::numeric_limits<double>::quiet_NaN()};
      double prominent_peak_hz_refined{std::numeric_limits<double>::quiet_NaN()};
      double prominent_peak_value_db{std::numeric_limits<double>::quiet_NaN()};
      double prominent_peak_fwhm_hz{std::numeric_limits<double>::quiet_NaN()};
      double prominent_peak_q{std::numeric_limits<double>::quiet_NaN()};
      double prominent_peak_prominence_db{std::numeric_limits<double>::quiet_NaN()};
      double alpha_peak_hz{std::numeric_limits<double>::quiet_NaN()};
      double alpha_peak_hz_refined{std::numeric_limits<double>::quiet_NaN()};
      double alpha_peak_value_db{std::numeric_limits<double>::quiet_NaN()};
      double alpha_fwhm_hz{std::numeric_limits<double>::quiet_NaN()};
      double alpha_q{std::numeric_limits<double>::quiet_NaN()};
      double alpha_prominence_db{std::numeric_limits<double>::quiet_NaN()};
      double median_hz{0.0};
      std::vector<double> edge_hzs;
      double periodic_median_hz{std::numeric_limits<double>::quiet_NaN()};
      std::vector<double> periodic_edge_hzs;
      double aperiodic_offset{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_exponent{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_r2{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_rmse{std::numeric_limits<double>::quiet_NaN()};
      std::size_t aperiodic_n_points{0};
      double aperiodic_slope{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_offset_db{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_aic{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_bic{std::numeric_limits<double>::quiet_NaN()};
      std::string aperiodic_best_model_aic{"na"};
      std::string aperiodic_best_model_bic{"na"};

      // Model comparison diagnostics (relative to the best/lowest score).
      double aperiodic_delta_aic_loglog{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_aic_weight_loglog{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_delta_bic_loglog{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_bic_weight_loglog{std::numeric_limits<double>::quiet_NaN()};

      // These are populated when the corresponding models are enabled.
      double aperiodic_delta_aic_two_slope{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_aic_weight_two_slope{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_delta_bic_two_slope{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_bic_weight_two_slope{std::numeric_limits<double>::quiet_NaN()};

      double aperiodic_delta_aic_knee{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_aic_weight_knee{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_delta_bic_knee{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_bic_weight_knee{std::numeric_limits<double>::quiet_NaN()};

      // Background model actually used for prominence / periodic residual metrics (after fallbacks).
      std::string aperiodic_background_used{"loglog"};


      // Optional: two-slope aperiodic fit (knee + low/high slopes).
      double aperiodic_knee_hz{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_slope_low{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_slope_high{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_exponent_low{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_exponent_high{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_r2_two_slope{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_rmse_two_slope{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_aic_two_slope{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_bic_two_slope{std::numeric_limits<double>::quiet_NaN()};

      // Optional curved knee aperiodic model (offset - log10(knee + f^exponent)).
      double aperiodic_offset_knee{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_exponent_knee{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_knee_param{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_knee_freq_hz{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_r2_knee{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_rmse_knee{std::numeric_limits<double>::quiet_NaN()};
      std::size_t aperiodic_n_points_knee{0};
      double aperiodic_aic_knee{std::numeric_limits<double>::quiet_NaN()};
      double aperiodic_bic_knee{std::numeric_limits<double>::quiet_NaN()};
      double periodic_power{std::numeric_limits<double>::quiet_NaN()};
      double periodic_rel{std::numeric_limits<double>::quiet_NaN()};

      std::vector<double> band_powers;
      std::vector<double> band_rels;
      std::vector<double> periodic_band_powers;
      std::vector<double> periodic_band_rels;
      std::vector<double> periodic_band_fracs;
      std::vector<double> band_prominent_peak_hzs;
      std::vector<double> band_prominent_peak_hz_refineds;
      std::vector<double> band_prominent_peak_value_dbs;
      std::vector<double> band_prominent_peak_fwhm_hzs;
      std::vector<double> band_prominent_peak_qs;
      std::vector<double> band_prominent_peak_prominence_dbs;
      std::vector<double> band_ratios;
    };
    std::vector<Row> rows;
    rows.reserve(rec.n_channels());

    for (size_t c = 0; c < rec.n_channels(); ++c) {
      PsdResult psd = welch_psd(rec.data[c], rec.fs_hz, wopt);
      Row r;
      r.ch = rec.channel_names[c];
      r.total_power = spectral_total_power(psd, fmin, fmax);
      r.entropy = spectral_entropy(psd, fmin, fmax, /*normalize=*/true);
      r.mean_hz = spectral_mean_frequency(psd, fmin, fmax);
      r.bandwidth_hz = spectral_bandwidth(psd, fmin, fmax);
      r.skewness = spectral_skewness(psd, fmin, fmax);
      r.kurtosis_excess = spectral_kurtosis_excess(psd, fmin, fmax);
      r.flatness = spectral_flatness(psd, fmin, fmax);
      r.peak_hz = spectral_peak_frequency(psd, fmin, fmax);
      r.peak_hz_refined = spectral_peak_frequency_parabolic(psd, fmin, fmax, /*log_domain=*/true);
      r.peak_value_db = spectral_value_db(psd, r.peak_hz);
      r.peak_fwhm_hz = spectral_peak_fwhm_hz(psd, r.peak_hz, fmin, fmax);
      if (std::isfinite(r.peak_fwhm_hz) && r.peak_fwhm_hz > 1e-12 && std::isfinite(r.peak_hz)) {
        r.peak_q = r.peak_hz / r.peak_fwhm_hz;
      }
      r.median_hz = spectral_edge_frequency(psd, fmin, fmax, 0.5);
      r.edge_hzs.clear();
      r.edge_hzs.reserve(edges_used.size());
      for (double e : edges_used) {
        r.edge_hzs.push_back(spectral_edge_frequency(psd, fmin, fmax, e));
      }

      const SpectralLogLogFit fit = spectral_loglog_fit(psd, a_fmin, a_fmax, aperiodic_excludes_used, /*robust=*/true);
      if (std::isfinite(fit.intercept)) {
        r.aperiodic_offset = fit.intercept;
        r.aperiodic_offset_db = 10.0 * fit.intercept;
      }
      if (std::isfinite(fit.slope)) {
        r.aperiodic_slope = fit.slope;
        r.aperiodic_exponent = -fit.slope;
      }
      if (std::isfinite(fit.r2)) r.aperiodic_r2 = fit.r2;
      if (std::isfinite(fit.rmse)) r.aperiodic_rmse = fit.rmse;
      r.aperiodic_n_points = fit.n_points;
      {
        const InfoCriteria ic = info_criteria_from_rmse(fit.rmse_unweighted, fit.n_points, /*num_params=*/2);
        r.aperiodic_aic = ic.aic;
        r.aperiodic_bic = ic.bic;
      }

      SpectralLogLogTwoSlopeFit fit2{};
      bool have_fit2 = false;
      SpectralAperiodicKneeFit kfit{};
      bool have_kfit = false;

      if (args.include_aperiodic_two_slope) {
        fit2 = spectral_loglog_two_slope_fit(psd,
                                                                             a_fmin,
                                                                             a_fmax,
                                                                             aperiodic_excludes_used,
                                                                             /*robust=*/true,
                                                                             /*max_iter=*/8,
                                                                             args.aperiodic_two_slope_min_points_per_side);
        have_fit2 = fit2.found;
        if (fit2.found) {
          r.aperiodic_knee_hz = fit2.knee_hz;
        } else {
          r.aperiodic_knee_hz = std::numeric_limits<double>::quiet_NaN();
        }
        r.aperiodic_slope_low = fit2.slope_low;
        r.aperiodic_slope_high = fit2.slope_high;
        if (std::isfinite(fit2.slope_low)) r.aperiodic_exponent_low = -fit2.slope_low;
        if (std::isfinite(fit2.slope_high)) r.aperiodic_exponent_high = -fit2.slope_high;
        if (std::isfinite(fit2.r2)) r.aperiodic_r2_two_slope = fit2.r2;
        if (std::isfinite(fit2.rmse)) r.aperiodic_rmse_two_slope = fit2.rmse;
        {
          const InfoCriteria ic2 = info_criteria_from_rmse(fit2.rmse_unweighted, fit2.n_points, /*num_params=*/4);
          r.aperiodic_aic_two_slope = ic2.aic;
          r.aperiodic_bic_two_slope = ic2.bic;
        }
      }

      if (args.include_aperiodic_knee_model) {
        kfit = spectral_aperiodic_knee_fit(psd,
                                                                          a_fmin,
                                                                          a_fmax,
                                                                          aperiodic_excludes_used,
                                                                          /*robust=*/true,
                                                                          /*max_iter=*/4);
        have_kfit = kfit.found;
        r.aperiodic_n_points_knee = kfit.n_points;
        if (kfit.found) {
          r.aperiodic_offset_knee = kfit.offset;
          r.aperiodic_exponent_knee = kfit.exponent;
          r.aperiodic_knee_param = kfit.knee;
          r.aperiodic_knee_freq_hz = kfit.knee_freq_hz;
          r.aperiodic_r2_knee = kfit.r2;
          r.aperiodic_rmse_knee = kfit.rmse;
          {
            int k_params = 3;
            if (std::isfinite(kfit.knee_freq_hz) && kfit.knee_freq_hz <= 0.0) {
              // knee_freq_hz==0 collapses to a straight 1/f^k (2 parameters in this model).
              k_params = 2;
            }
            const InfoCriteria ic3 = info_criteria_from_rmse(kfit.rmse_unweighted, kfit.n_points, k_params);
            r.aperiodic_aic_knee = ic3.aic;
            r.aperiodic_bic_knee = ic3.bic;
          }
        }
      }

      // Select best aperiodic model by information criteria (lowest wins).
      r.aperiodic_best_model_aic = best_model_from_score(r.aperiodic_aic, r.aperiodic_aic_two_slope, r.aperiodic_aic_knee);
      r.aperiodic_best_model_bic = best_model_from_score(r.aperiodic_bic, r.aperiodic_bic_two_slope, r.aperiodic_bic_knee);

      // Compute model comparison diagnostics (Δ and normalized weights) from AIC/BIC.
      {
        const ModelDeltasWeights aicw = model_deltas_and_weights(r.aperiodic_aic, r.aperiodic_aic_two_slope, r.aperiodic_aic_knee);
        r.aperiodic_delta_aic_loglog = aicw.delta_loglog;
        r.aperiodic_aic_weight_loglog = aicw.weight_loglog;
        if (args.include_aperiodic_two_slope) {
          r.aperiodic_delta_aic_two_slope = aicw.delta_two_slope;
          r.aperiodic_aic_weight_two_slope = aicw.weight_two_slope;
        }
        if (args.include_aperiodic_knee_model) {
          r.aperiodic_delta_aic_knee = aicw.delta_knee;
          r.aperiodic_aic_weight_knee = aicw.weight_knee;
        }

        const ModelDeltasWeights bicw = model_deltas_and_weights(r.aperiodic_bic, r.aperiodic_bic_two_slope, r.aperiodic_bic_knee);
        r.aperiodic_delta_bic_loglog = bicw.delta_loglog;
        r.aperiodic_bic_weight_loglog = bicw.weight_loglog;
        if (args.include_aperiodic_two_slope) {
          r.aperiodic_delta_bic_two_slope = bicw.delta_two_slope;
          r.aperiodic_bic_weight_two_slope = bicw.weight_two_slope;
        }
        if (args.include_aperiodic_knee_model) {
          r.aperiodic_delta_bic_knee = bicw.delta_knee;
          r.aperiodic_bic_weight_knee = bicw.weight_knee;
        }
      }

      // Select aperiodic background model for prominence / periodic residual metrics.
      // Default is the legacy single-slope log-log fit; auto modes select per-channel based on AIC/BIC.
      std::string bg_model = "loglog";
      switch (args.aperiodic_background) {
        case AperiodicBackgroundMode::LogLog: bg_model = "loglog"; break;
        case AperiodicBackgroundMode::TwoSlope: bg_model = "two_slope"; break;
        case AperiodicBackgroundMode::Knee: bg_model = "knee"; break;
        case AperiodicBackgroundMode::AutoAIC: bg_model = r.aperiodic_best_model_aic; break;
        case AperiodicBackgroundMode::AutoBIC: bg_model = r.aperiodic_best_model_bic; break;
      }

      // Fall back to loglog if the requested model isn't available for this channel.
      if (bg_model == "two_slope" && !(args.include_aperiodic_two_slope && have_fit2)) {
        bg_model = "loglog";
      }
      if (bg_model == "knee" && !(args.include_aperiodic_knee_model && have_kfit)) {
        bg_model = "loglog";
      }
      if (!(bg_model == "loglog" || bg_model == "two_slope" || bg_model == "knee")) {
        bg_model = "loglog";
      }

      r.aperiodic_background_used = bg_model;

      auto periodic_power_range = [&](double lo_hz, double hi_hz) -> double {
        if (bg_model == "two_slope") {
          return spectral_periodic_power_from_two_slope_fit(psd, lo_hz, hi_hz, fit2, /*positive_only=*/true);
        } else if (bg_model == "knee") {
          return spectral_periodic_power_from_knee_fit(psd, lo_hz, hi_hz, kfit, /*positive_only=*/true);
        }
        return spectral_periodic_power_from_loglog_fit(psd, lo_hz, hi_hz, fit, /*positive_only=*/true);
      };

      auto periodic_power_fraction_range = [&](double lo_hz, double hi_hz) -> double {
        if (bg_model == "two_slope") {
          return spectral_periodic_power_fraction_from_two_slope_fit(psd, lo_hz, hi_hz, fit2, /*positive_only=*/true);
        } else if (bg_model == "knee") {
          return spectral_periodic_power_fraction_from_knee_fit(psd, lo_hz, hi_hz, kfit, /*positive_only=*/true);
        }
        return spectral_periodic_power_fraction_from_loglog_fit(psd, lo_hz, hi_hz, fit, /*positive_only=*/true);
      };

      auto periodic_edge_frequency_range = [&](double lo_hz, double hi_hz, double edge) -> double {
        if (bg_model == "two_slope") {
          return spectral_periodic_edge_frequency_from_two_slope_fit(psd, lo_hz, hi_hz, fit2, edge);
        } else if (bg_model == "knee") {
          return spectral_periodic_edge_frequency_from_knee_fit(psd, lo_hz, hi_hz, kfit, edge);
        }
        return spectral_periodic_edge_frequency_from_loglog_fit(psd, lo_hz, hi_hz, fit, edge);
      };

      auto prominence_db_at = [&](double freq_hz) -> double {
        if (bg_model == "two_slope") {
          return spectral_prominence_db_from_two_slope_fit(psd, freq_hz, fit2);
        } else if (bg_model == "knee") {
          return spectral_prominence_db_from_knee_fit(psd, freq_hz, kfit);
        }
        return spectral_prominence_db_from_loglog_fit(psd, freq_hz, fit);
      };

      auto max_prominence_peak = [&](double lo_hz, double hi_hz) -> SpectralProminentPeak {
        if (bg_model == "two_slope") {
          return spectral_max_prominence_peak(psd, lo_hz, hi_hz, fit2, /*require_local_max=*/true, /*min_prominence_db=*/0.0);
        } else if (bg_model == "knee") {
          return spectral_max_prominence_peak(psd, lo_hz, hi_hz, kfit, /*require_local_max=*/true, /*min_prominence_db=*/0.0);
        }
        return spectral_max_prominence_peak(psd, lo_hz, hi_hz, fit, /*require_local_max=*/true, /*min_prominence_db=*/0.0);
      };

      // Periodic (oscillatory) power above the selected aperiodic background.
      r.periodic_power = periodic_power_range(fmin, fmax);
      r.periodic_rel = periodic_power_fraction_range(fmin, fmax);

      // Periodic edge frequencies (aperiodic-adjusted residual).
      r.periodic_median_hz = periodic_edge_frequency_range(fmin, fmax, 0.5);
      r.periodic_edge_hzs.clear();
      r.periodic_edge_hzs.reserve(edges_used.size());
      for (double e : edges_used) {
        r.periodic_edge_hzs.push_back(periodic_edge_frequency_range(fmin, fmax, e));
      }

      // Peak prominences relative to the selected aperiodic background model.
      r.peak_prominence_db = prominence_db_at(r.peak_hz);

      // Most prominent oscillatory peak (max prominence above aperiodic fit).
      const SpectralProminentPeak pp = max_prominence_peak(fmin, fmax);
      if (pp.found) {
        r.prominent_peak_hz = pp.peak_hz;
        r.prominent_peak_hz_refined = pp.peak_hz_refined;
        r.prominent_peak_prominence_db = pp.prominence_db;
        r.prominent_peak_value_db = spectral_value_db(psd, r.prominent_peak_hz);
        r.prominent_peak_fwhm_hz = spectral_peak_fwhm_hz(psd, r.prominent_peak_hz, fmin, fmax);
        if (std::isfinite(r.prominent_peak_fwhm_hz) && r.prominent_peak_fwhm_hz > 1e-12 &&
            std::isfinite(r.prominent_peak_hz)) {
          r.prominent_peak_q = r.prominent_peak_hz / r.prominent_peak_fwhm_hz;
        }
      }

      const double alpha_lo = std::max(fmin, args.alpha_min_hz);
      const double alpha_hi = std::min(fmax, args.alpha_max_hz);
      if (alpha_hi > alpha_lo) {
        r.alpha_peak_hz = spectral_peak_frequency(psd, alpha_lo, alpha_hi);
        r.alpha_peak_hz_refined = spectral_peak_frequency_parabolic(psd, alpha_lo, alpha_hi, /*log_domain=*/true);
        r.alpha_peak_value_db = spectral_value_db(psd, r.alpha_peak_hz);
        r.alpha_fwhm_hz = spectral_peak_fwhm_hz(psd, r.alpha_peak_hz, alpha_lo, alpha_hi);
        if (std::isfinite(r.alpha_fwhm_hz) && r.alpha_fwhm_hz > 1e-12 && std::isfinite(r.alpha_peak_hz)) {
          r.alpha_q = r.alpha_peak_hz / r.alpha_fwhm_hz;
        }
        r.alpha_prominence_db = prominence_db_at(r.alpha_peak_hz);
      }

      // Bandpowers + relative bandpowers (+ optional periodic bandpowers above the aperiodic fit).
      if (!bands.empty()) {
        r.band_powers.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
        r.band_rels.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
        if (include_periodic_bands) {
          r.periodic_band_powers.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
          r.periodic_band_rels.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
          r.periodic_band_fracs.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
        }
        if (include_band_peaks) {
          r.band_prominent_peak_hzs.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
          r.band_prominent_peak_hz_refineds.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
          r.band_prominent_peak_value_dbs.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
          r.band_prominent_peak_fwhm_hzs.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
          r.band_prominent_peak_qs.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
          r.band_prominent_peak_prominence_dbs.assign(bands.size(), std::numeric_limits<double>::quiet_NaN());
        }
        const double denom_total = r.total_power;
        const double denom_periodic = r.periodic_power;
        for (size_t bi = 0; bi < bands.size(); ++bi) {
          const double blo = std::max(fmin, bands[bi].lo_hz);
          const double bhi = std::min(fmax, bands[bi].hi_hz);
          if (bhi > blo) {
            const double bp = spectral_total_power(psd, blo, bhi);
            r.band_powers[bi] = bp;
            if (std::isfinite(bp) && std::isfinite(denom_total) && denom_total > 1e-20) {
              r.band_rels[bi] = bp / denom_total;
            }

            if (include_periodic_bands) {
              const double ppow = periodic_power_range(blo, bhi);
              r.periodic_band_powers[bi] = ppow;
              if (std::isfinite(ppow) && std::isfinite(denom_total) && denom_total > 1e-20) {
                r.periodic_band_rels[bi] = ppow / denom_total;
              }
              if (std::isfinite(ppow) && std::isfinite(denom_periodic) && denom_periodic > 1e-20) {
                r.periodic_band_fracs[bi] = ppow / denom_periodic;
              }
            }

            // Most prominent peak within the band (max prominence above aperiodic fit).
            if (include_band_peaks) {
              const SpectralProminentPeak bpp = max_prominence_peak(blo, bhi);
              if (bpp.found) {
                r.band_prominent_peak_hzs[bi] = bpp.peak_hz;
                r.band_prominent_peak_hz_refineds[bi] = bpp.peak_hz_refined;
                r.band_prominent_peak_prominence_dbs[bi] = bpp.prominence_db;
                r.band_prominent_peak_value_dbs[bi] = spectral_value_db(psd, bpp.peak_hz);
                r.band_prominent_peak_fwhm_hzs[bi] = spectral_peak_fwhm_hz(psd, bpp.peak_hz, blo, bhi);
                if (std::isfinite(r.band_prominent_peak_fwhm_hzs[bi]) &&
                    r.band_prominent_peak_fwhm_hzs[bi] > 1e-12 &&
                    std::isfinite(r.band_prominent_peak_hzs[bi])) {
                  r.band_prominent_peak_qs[bi] = r.band_prominent_peak_hzs[bi] / r.band_prominent_peak_fwhm_hzs[bi];
                }
              }
            }
          }
        }
      }

      // Ratios.
      if (!ratios.empty()) {
        r.band_ratios.assign(ratios.size(), std::numeric_limits<double>::quiet_NaN());
        for (size_t ri = 0; ri < ratios.size(); ++ri) {
          const auto itn = band_index.find(ratios[ri].num_key);
          const auto itd = band_index.find(ratios[ri].den_key);
          if (itn == band_index.end() || itd == band_index.end()) continue;
          const double num = r.band_powers[itn->second];
          const double den = r.band_powers[itd->second];
          if (std::isfinite(num) && std::isfinite(den) && den > 1e-20) {
            r.band_ratios[ri] = num / den;
          }
        }
      }

      rows.push_back(r);
    }

    // Write CSV
    {
      const std::string csv_path = args.outdir + "/spectral_features.csv";
      std::ofstream out(std::filesystem::u8path(csv_path), std::ios::binary);
      if (!out) throw std::runtime_error("Failed to write spectral_features.csv: " + csv_path);

      std::vector<std::string> header;
      header.push_back("channel");
      header.push_back("total_power");
      header.push_back("entropy");
      header.push_back("mean_hz");
      header.push_back("bandwidth_hz");
      header.push_back("skewness");
      header.push_back("kurtosis_excess");
      header.push_back("flatness");
      header.push_back("peak_hz");
      header.push_back("peak_hz_refined");
      header.push_back("peak_value_db");
      header.push_back("peak_fwhm_hz");
      header.push_back("peak_q");
      header.push_back("peak_prominence_db");
      header.push_back("prominent_peak_hz");
      header.push_back("prominent_peak_hz_refined");
      header.push_back("prominent_peak_value_db");
      header.push_back("prominent_peak_fwhm_hz");
      header.push_back("prominent_peak_q");
      header.push_back("prominent_peak_prominence_db");
      header.push_back("alpha_peak_hz");
      header.push_back("alpha_peak_hz_refined");
      header.push_back("alpha_peak_value_db");
      header.push_back("alpha_fwhm_hz");
      header.push_back("alpha_q");
      header.push_back("alpha_prominence_db");
      header.push_back("median_hz");
      for (const auto& c : edge_cols) {
        header.push_back(c);
      }
      header.push_back("periodic_median_hz");
      for (const auto& c : periodic_edge_cols) {
        header.push_back(c);
      }
      header.push_back("aperiodic_offset");
      header.push_back("aperiodic_exponent");
      header.push_back("aperiodic_r2");
      header.push_back("periodic_power");
      header.push_back("periodic_rel");
      header.push_back("aperiodic_rmse");
      header.push_back("aperiodic_n_points");
      header.push_back("aperiodic_slope");
      header.push_back("aperiodic_offset_db");
      header.push_back("aperiodic_aic");
      header.push_back("aperiodic_bic");

      if (args.include_aperiodic_two_slope) {
        header.push_back("aperiodic_knee_hz");
        header.push_back("aperiodic_slope_low");
        header.push_back("aperiodic_slope_high");
        header.push_back("aperiodic_exponent_low");
        header.push_back("aperiodic_exponent_high");
        header.push_back("aperiodic_r2_two_slope");
        header.push_back("aperiodic_rmse_two_slope");
        header.push_back("aperiodic_aic_two_slope");
        header.push_back("aperiodic_bic_two_slope");
      }

      if (args.include_aperiodic_knee_model) {
        header.push_back("aperiodic_offset_knee");
        header.push_back("aperiodic_exponent_knee");
        header.push_back("aperiodic_knee_param");
        header.push_back("aperiodic_knee_freq_hz");
        header.push_back("aperiodic_r2_knee");
        header.push_back("aperiodic_rmse_knee");
        header.push_back("aperiodic_n_points_knee");
        header.push_back("aperiodic_aic_knee");
        header.push_back("aperiodic_bic_knee");
      }
      header.push_back("aperiodic_best_model_aic");
      header.push_back("aperiodic_best_model_bic");

      header.push_back("aperiodic_delta_aic_loglog");
      header.push_back("aperiodic_aic_weight_loglog");
      if (args.include_aperiodic_two_slope) {
        header.push_back("aperiodic_delta_aic_two_slope");
        header.push_back("aperiodic_aic_weight_two_slope");
      }
      if (args.include_aperiodic_knee_model) {
        header.push_back("aperiodic_delta_aic_knee");
        header.push_back("aperiodic_aic_weight_knee");
      }
      header.push_back("aperiodic_delta_bic_loglog");
      header.push_back("aperiodic_bic_weight_loglog");
      if (args.include_aperiodic_two_slope) {
        header.push_back("aperiodic_delta_bic_two_slope");
        header.push_back("aperiodic_bic_weight_two_slope");
      }
      if (args.include_aperiodic_knee_model) {
        header.push_back("aperiodic_delta_bic_knee");
        header.push_back("aperiodic_bic_weight_knee");
      }
      header.push_back("aperiodic_background_used");

      for (const auto& b : bands) {
        header.push_back(b.col_power);
        header.push_back(b.col_rel);
        if (include_periodic_bands) {
          header.push_back(b.col_periodic_power);
          header.push_back(b.col_periodic_rel);
          header.push_back(b.col_periodic_frac);
        }
        if (include_band_peaks) {
          header.push_back(b.col_prominent_peak_hz);
          header.push_back(b.col_prominent_peak_hz_refined);
          header.push_back(b.col_prominent_peak_value_db);
          header.push_back(b.col_prominent_peak_fwhm_hz);
          header.push_back(b.col_prominent_peak_q);
          header.push_back(b.col_prominent_peak_prominence_db);
        }
      }
      for (const auto& r : ratios) {
        header.push_back(r.col);
      }

      for (size_t i = 0; i < header.size(); ++i) {
        out << header[i];
        out << (i + 1 < header.size() ? ',' : '\n');
      }
      out << std::setprecision(12);
      for (const auto& r : rows) {
        out << r.ch << "," << r.total_power << "," << r.entropy << "," << r.mean_hz << "," << r.bandwidth_hz << ","
            << r.skewness << "," << r.kurtosis_excess << "," << r.flatness << "," << r.peak_hz << ","
            << r.peak_hz_refined << "," << r.peak_value_db << "," << r.peak_fwhm_hz << "," << r.peak_q << ","
            << r.peak_prominence_db << "," << r.prominent_peak_hz << "," << r.prominent_peak_hz_refined << ","
            << r.prominent_peak_value_db << "," << r.prominent_peak_fwhm_hz << "," << r.prominent_peak_q << ","
            << r.prominent_peak_prominence_db << "," << r.alpha_peak_hz << "," << r.alpha_peak_hz_refined << ","
            << r.alpha_peak_value_db << "," << r.alpha_fwhm_hz << "," << r.alpha_q << "," << r.alpha_prominence_db
            << "," << r.median_hz;
        for (double v : r.edge_hzs) {
          out << "," << v;
        }
        out << "," << r.periodic_median_hz;
        for (double v : r.periodic_edge_hzs) {
          out << "," << v;
        }
        out << "," << r.aperiodic_offset << "," << r.aperiodic_exponent
            << "," << r.aperiodic_r2 << "," << r.periodic_power << "," << r.periodic_rel
            << "," << r.aperiodic_rmse << "," << r.aperiodic_n_points << "," << r.aperiodic_slope << "," << r.aperiodic_offset_db            << "," << r.aperiodic_aic << "," << r.aperiodic_bic;

        if (args.include_aperiodic_two_slope) {
          out << "," << r.aperiodic_knee_hz << "," << r.aperiodic_slope_low << "," << r.aperiodic_slope_high
              << "," << r.aperiodic_exponent_low << "," << r.aperiodic_exponent_high
              << "," << r.aperiodic_r2_two_slope << "," << r.aperiodic_rmse_two_slope              << "," << r.aperiodic_aic_two_slope << "," << r.aperiodic_bic_two_slope;
        }

        if (args.include_aperiodic_knee_model) {
          out << "," << r.aperiodic_offset_knee << "," << r.aperiodic_exponent_knee << "," << r.aperiodic_knee_param
              << "," << r.aperiodic_knee_freq_hz << "," << r.aperiodic_r2_knee << "," << r.aperiodic_rmse_knee
              << "," << r.aperiodic_n_points_knee              << "," << r.aperiodic_aic_knee << "," << r.aperiodic_bic_knee;
        }

        out << "," << r.aperiodic_best_model_aic << "," << r.aperiodic_best_model_bic;

        out << "," << r.aperiodic_delta_aic_loglog << "," << r.aperiodic_aic_weight_loglog;
        if (args.include_aperiodic_two_slope) {
          out << "," << r.aperiodic_delta_aic_two_slope << "," << r.aperiodic_aic_weight_two_slope;
        }
        if (args.include_aperiodic_knee_model) {
          out << "," << r.aperiodic_delta_aic_knee << "," << r.aperiodic_aic_weight_knee;
        }
        out << "," << r.aperiodic_delta_bic_loglog << "," << r.aperiodic_bic_weight_loglog;
        if (args.include_aperiodic_two_slope) {
          out << "," << r.aperiodic_delta_bic_two_slope << "," << r.aperiodic_bic_weight_two_slope;
        }
        if (args.include_aperiodic_knee_model) {
          out << "," << r.aperiodic_delta_bic_knee << "," << r.aperiodic_bic_weight_knee;
        }
        out << "," << r.aperiodic_background_used;

        for (size_t bi = 0; bi < bands.size(); ++bi) {
          const double bp = (bi < r.band_powers.size()) ? r.band_powers[bi] : std::numeric_limits<double>::quiet_NaN();
          const double br = (bi < r.band_rels.size()) ? r.band_rels[bi] : std::numeric_limits<double>::quiet_NaN();
          out << "," << bp << "," << br;
          if (include_periodic_bands) {
            const double pp = (bi < r.periodic_band_powers.size()) ? r.periodic_band_powers[bi] : std::numeric_limits<double>::quiet_NaN();
            const double pr = (bi < r.periodic_band_rels.size()) ? r.periodic_band_rels[bi] : std::numeric_limits<double>::quiet_NaN();
            const double pf = (bi < r.periodic_band_fracs.size()) ? r.periodic_band_fracs[bi] : std::numeric_limits<double>::quiet_NaN();
            out << "," << pp << "," << pr << "," << pf;
          }
          if (include_band_peaks) {
            const double phz = (bi < r.band_prominent_peak_hzs.size()) ? r.band_prominent_peak_hzs[bi] : std::numeric_limits<double>::quiet_NaN();
            const double phzr = (bi < r.band_prominent_peak_hz_refineds.size()) ? r.band_prominent_peak_hz_refineds[bi] : std::numeric_limits<double>::quiet_NaN();
            const double pvdb = (bi < r.band_prominent_peak_value_dbs.size()) ? r.band_prominent_peak_value_dbs[bi] : std::numeric_limits<double>::quiet_NaN();
            const double pfwhm = (bi < r.band_prominent_peak_fwhm_hzs.size()) ? r.band_prominent_peak_fwhm_hzs[bi] : std::numeric_limits<double>::quiet_NaN();
            const double pq = (bi < r.band_prominent_peak_qs.size()) ? r.band_prominent_peak_qs[bi] : std::numeric_limits<double>::quiet_NaN();
            const double pprom = (bi < r.band_prominent_peak_prominence_dbs.size()) ? r.band_prominent_peak_prominence_dbs[bi] : std::numeric_limits<double>::quiet_NaN();
            out << "," << phz << "," << phzr << "," << pvdb << "," << pfwhm << "," << pq << "," << pprom;
          }
        }
        for (size_t ri = 0; ri < ratios.size(); ++ri) {
          const double rv = (ri < r.band_ratios.size()) ? r.band_ratios[ri] : std::numeric_limits<double>::quiet_NaN();
          out << "," << rv;
        }
        out << "\n";
      }
    }

    // JSON sidecar describing columns
    write_sidecar_json(args);

    // JSON file capturing the parameters used for reproducibility
    write_params_json(args, rec, popt, wopt, fmin, fmax, a_fmin, a_fmax, aperiodic_excludes_used, edges_used, bands, ratios, include_periodic_bands, include_band_peaks);

    // Run meta for qeeg_ui_cli
    {
      const std::string meta_path = args.outdir + "/spectral_features_run_meta.json";
      std::vector<std::string> outs;
      outs.push_back("spectral_features.csv");
      outs.push_back("spectral_features.json");
      outs.push_back("spectral_features_params.json");
      outs.push_back("spectral_features_run_meta.json");
      if (!write_run_meta_json(meta_path, "qeeg_spectral_features_cli", args.outdir, args.input_path, outs)) {
        std::cerr << "Warning: failed to write run meta JSON: " << meta_path << "\n";
      }
    }

    std::cout << "Wrote: " << args.outdir << "/spectral_features.csv\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
