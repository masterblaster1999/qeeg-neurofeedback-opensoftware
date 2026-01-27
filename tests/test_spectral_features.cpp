#include "qeeg/spectral_features.hpp"

#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <vector>

static bool approx(double a, double b, double eps = 1e-8) {
  return std::fabs(a - b) <= eps;
}

int main() {
  using namespace qeeg;

  // Case 1: constant PSD on [0,10].
  // - Total power should be width.
  // - Normalized entropy should be 1 (uniform distribution across frequency).
  // - SEF95 should be 9.5.
  PsdResult flat;
  for (int i = 0; i <= 10; ++i) {
    flat.freqs_hz.push_back(static_cast<double>(i));
    flat.psd.push_back(1.0);
  }

  const double total = spectral_total_power(flat, 0.0, 10.0);
  assert(approx(total, 10.0));

  const double H = spectral_entropy(flat, 0.0, 10.0, /*normalize=*/true);
  assert(approx(H, 1.0, 1e-12));

  const double bw = spectral_bandwidth(flat, 0.0, 10.0);
  const double expected_bw = 10.0 / std::sqrt(12.0); // stddev of Uniform[0,10]
  assert(approx(bw, expected_bw, 1e-12));

  const double flatness = spectral_flatness(flat, 0.0, 10.0);
  assert(approx(flatness, 1.0, 1e-12));

  const double skew1 = spectral_skewness(flat, 0.0, 10.0);
  assert(approx(skew1, 0.0, 1e-12));

  const double kurt1 = spectral_kurtosis_excess(flat, 0.0, 10.0);
  assert(approx(kurt1, -1.2, 1e-12));

  const double sef95 = spectral_edge_frequency(flat, 0.0, 10.0, 0.95);
  assert(approx(sef95, 9.5, 1e-12));

  const double med = spectral_edge_frequency(flat, 0.0, 10.0, 0.5);
  assert(approx(med, 5.0, 1e-12));

  // Case 2: PSD proportional to frequency: P(f)=f.
  // Total power = ∫_0^10 f df = 50.
  // Median frequency solves ∫_0^x f df = 25 => x = sqrt(50).
  PsdResult ramp;
  for (int i = 0; i <= 10; ++i) {
    ramp.freqs_hz.push_back(static_cast<double>(i));
    ramp.psd.push_back(static_cast<double>(i));
  }
  const double total2 = spectral_total_power(ramp, 0.0, 10.0);
  assert(approx(total2, 50.0, 1e-12));

  const double med2 = spectral_edge_frequency(ramp, 0.0, 10.0, 0.5);
  const double expected_med2 = std::sqrt(50.0);
  assert(approx(med2, expected_med2, 1e-9));

  const double bw2 = spectral_bandwidth(ramp, 0.0, 10.0);
  const double expected_bw2 = std::sqrt(50.0 / 9.0);
  assert(approx(bw2, expected_bw2, 1e-9));

  const double flatness2 = spectral_flatness(ramp, 1.0, 10.0);
  assert(flatness2 > 0.0);
  assert(flatness2 < 1.0);

  const double skew2 = spectral_skewness(ramp, 0.0, 10.0);
  const double expected_skew2 = -4.0 / (5.0 * std::sqrt(2.0));
  assert(approx(skew2, expected_skew2, 1e-9));

  const double kurt2 = spectral_kurtosis_excess(ramp, 0.0, 10.0);
  assert(approx(kurt2, -0.6, 1e-9));

  // Case 3: power-law PSD: P(f) = 1 / f^2 on [1,10].
  // In log10 space: log10(P) = -2 * log10(f), so slope=-2, exponent=2, intercept=0.
  PsdResult powlaw;
  for (int i = 1; i <= 10; ++i) {
    const double f = static_cast<double>(i);
    powlaw.freqs_hz.push_back(f);
    powlaw.psd.push_back(1.0 / (f * f));
  }

  const SpectralLogLogFit fit = spectral_loglog_fit(powlaw, 1.0, 10.0, /*robust=*/true);
  assert(std::isfinite(fit.slope));
  assert(std::isfinite(fit.intercept));
  assert(std::isfinite(fit.r2));
  assert(approx(fit.slope, -2.0, 1e-10));
  assert(approx(-fit.slope, 2.0, 1e-10));
  assert(approx(fit.intercept, 0.0, 1e-10));
  assert(approx(fit.r2, 1.0, 1e-12));

  assert(std::isfinite(fit.rmse));
  assert(approx(fit.rmse, 0.0, 1e-12));

  assert(std::isfinite(fit.rmse_unweighted));
  assert(approx(fit.rmse_unweighted, 0.0, 1e-12));

  // A constant PSD should yield a ~0 slope in log-log space.
  const SpectralLogLogFit fit_flat = spectral_loglog_fit(flat, 1.0, 10.0, /*robust=*/true);
  assert(std::isfinite(fit_flat.slope));
  assert(approx(fit_flat.slope, 0.0, 1e-12));
  assert(std::isfinite(fit_flat.intercept));
  assert(approx(fit_flat.intercept, 0.0, 1e-12));

  assert(std::isfinite(fit_flat.rmse));
  assert(approx(fit_flat.rmse, 0.0, 1e-12));

  assert(std::isfinite(fit_flat.rmse_unweighted));
  assert(approx(fit_flat.rmse_unweighted, 0.0, 1e-12));

  // PSD interpolation helper.
  assert(approx(spectral_psd_at_frequency(flat, 0.5), 1.0, 1e-12));

  // Peak prominence vs aperiodic (log-log) fit.
  const double prom0 = spectral_prominence_db_from_loglog_fit(powlaw, 5.0, fit);
  assert(approx(prom0, 0.0, 1e-10));

  PsdResult powlaw_bump = powlaw;
  for (size_t i = 0; i < powlaw_bump.freqs_hz.size(); ++i) {
    if (approx(powlaw_bump.freqs_hz[i], 5.0)) {
      powlaw_bump.psd[i] *= 100.0; // +20 dB at 5 Hz
    }
  }
  const double prom20 = spectral_prominence_db_from_loglog_fit(powlaw_bump, 5.0, fit);
  assert(approx(prom20, 20.0, 1e-8));

  // Periodic (oscillatory) power above the aperiodic background.
  const double per0 = spectral_periodic_power_from_loglog_fit(powlaw, 1.0, 10.0, fit, /*positive_only=*/true);
  assert(approx(per0, 0.0, 1e-12));
  const double per0_rel = spectral_periodic_power_fraction_from_loglog_fit(powlaw, 1.0, 10.0, fit, /*positive_only=*/true);
  assert(approx(per0_rel, 0.0, 1e-12));

  const double per = spectral_periodic_power_from_loglog_fit(powlaw_bump, 1.0, 10.0, fit, /*positive_only=*/true);
  // At f=5 baseline=1/25=0.04, bump=4.0 => excess=3.96. With 1 Hz spacing, the
  // trapezoidal integral yields 0.5*3.96*1 (4->5) + 0.5*3.96*1 (5->6) = 3.96.
  assert(approx(per, 3.96, 1e-8));
  const double per_band_4_6 = spectral_periodic_power_from_loglog_fit(powlaw_bump, 4.0, 6.0, fit, /*positive_only=*/true);
  assert(approx(per_band_4_6, 3.96, 1e-8));
  const double per_rel = spectral_periodic_power_fraction_from_loglog_fit(powlaw_bump, 1.0, 10.0, fit, /*positive_only=*/true);
  const double total_bump = spectral_total_power(powlaw_bump, 1.0, 10.0);
  assert(approx(per_rel, per / total_bump, 1e-10));

  // Spectral edge frequencies on the periodic (aperiodic-adjusted) residual power.
  // For the single-bin bump at 5 Hz, the residual is triangular on [4,6] and symmetric.
  const double per_sef50 = spectral_periodic_edge_frequency_from_loglog_fit(powlaw_bump, 1.0, 10.0, fit, 0.5);
  assert(approx(per_sef50, 5.0, 1e-12));

  const double per_sef95 = spectral_periodic_edge_frequency_from_loglog_fit(powlaw_bump, 1.0, 10.0, fit, 0.95);
  const double expected_per_sef95 = 6.0 - std::sqrt(0.1); // derived from the symmetric triangle geometry
  assert(approx(per_sef95, expected_per_sef95, 1e-9));

  // No periodic component => NaN.
  const double per_sef_none = spectral_periodic_edge_frequency_from_loglog_fit(powlaw, 1.0, 10.0, fit, 0.5, 1e-12);
  assert(!std::isfinite(per_sef_none));

  // Most-prominent peak (max prominence) relative to log-log fit.
  const SpectralProminentPeak pp_none = spectral_max_prominence_peak(powlaw, 1.0, 10.0, fit,
                                                                    /*require_local_max=*/true,
                                                                    /*min_prominence_db=*/0.0);
  assert(!pp_none.found);

  const SpectralProminentPeak pp = spectral_max_prominence_peak(powlaw_bump, 1.0, 10.0, fit,
                                                               /*require_local_max=*/true,
                                                               /*min_prominence_db=*/0.0);
  assert(pp.found);
  assert(approx(pp.peak_hz, 5.0, 1e-12));
  assert(approx(pp.peak_hz_refined, 5.0, 1e-12));
  assert(approx(pp.prominence_db, 20.0, 1e-8));

  // Restricting the search range should still find the same peak if the range contains it.
  const SpectralProminentPeak pp_band = spectral_max_prominence_peak(powlaw_bump, 4.0, 6.0, fit,
                                                                    /*require_local_max=*/true,
                                                                    /*min_prominence_db=*/0.0);
  assert(pp_band.found);
  assert(approx(pp_band.peak_hz, 5.0, 1e-12));
  assert(approx(pp_band.peak_hz_refined, 5.0, 1e-12));

  // Excluding the bump should yield no prominent peak.
  const SpectralProminentPeak pp_miss = spectral_max_prominence_peak(powlaw_bump, 1.0, 4.0, fit,
                                                                    /*require_local_max=*/true,
                                                                    /*min_prominence_db=*/0.0);
  assert(!pp_miss.found);

  // Bandpower helpers.
  // Flat PSD: power equals the band width.
  const double bp_delta = spectral_band_power(flat, 1.0, 4.0);
  assert(approx(bp_delta, 3.0, 1e-12));
  const double bp_theta = spectral_band_power(flat, 4.0, 8.0);
  assert(approx(bp_theta, 4.0, 1e-12));
  const double rel_delta = spectral_relative_band_power(flat, 1.0, 4.0, 0.0, 10.0);
  assert(approx(rel_delta, 3.0 / 10.0, 1e-12));

  // Ramp PSD: P(f)=f. Bandpower over [0,5] is ∫ f df = 12.5.
  const double bp_ramp_0_5 = spectral_band_power(ramp, 0.0, 5.0);
  assert(approx(bp_ramp_0_5, 12.5, 1e-10));
  const double bp_ramp_5_10 = spectral_band_power(ramp, 5.0, 10.0);
  assert(approx(bp_ramp_5_10, 37.5, 1e-10));
  const double rel_ramp_0_5 = spectral_relative_band_power(ramp, 0.0, 5.0, 0.0, 10.0);
  assert(approx(rel_ramp_0_5, 12.5 / 50.0, 1e-10));

  // Case 4: quadratic peak refinement in log domain.
  // Construct a spectrum whose log10(PSD) is exactly quadratic with a vertex at 5.3 Hz.
  // Parabolic interpolation should recover the vertex (to numerical precision) from
  // the 3-point neighborhood.
  PsdResult quad;
  for (int i = 0; i <= 10; ++i) {
    const double f = static_cast<double>(i);
    quad.freqs_hz.push_back(f);
    const double y = -(f - 5.3) * (f - 5.3); // log10 power
    quad.psd.push_back(std::pow(10.0, y));
  }
  const double peak_bin = spectral_peak_frequency(quad, 0.0, 10.0);
  assert(approx(peak_bin, 5.0, 1e-12));
  const double peak_ref = spectral_peak_frequency_parabolic(quad, 0.0, 10.0, /*log_domain=*/true);
  assert(approx(peak_ref, 5.3, 1e-12));

  // Value in dB at the peak bin: 10 * log10(10^{-0.09}) = -0.9 dB.
  const double peak_db = spectral_value_db(quad, peak_bin);
  assert(approx(peak_db, -0.9, 1e-12));

  // Case 5: FWHM on a piecewise-linear (triangular) peak.
  // PSD: 0,1,2,1,0 on freqs 0..4 => half-max crossing at 1 and 3 => FWHM=2.
  PsdResult tri;
  tri.freqs_hz = {0.0, 1.0, 2.0, 3.0, 4.0};
  tri.psd = {0.0, 1.0, 2.0, 1.0, 0.0};
  const double tri_peak = spectral_peak_frequency(tri, 0.0, 4.0);
  assert(approx(tri_peak, 2.0, 1e-12));
  const double tri_fwhm = spectral_peak_fwhm_hz(tri, tri_peak, 0.0, 4.0);
  assert(approx(tri_fwhm, 2.0, 1e-12));



  // Case 6: aperiodic fit with excluded ranges.
  // Build an exact 1/f spectrum (k=1) and then add an enormous bump in the
  // aperiodic fit range. Excluding the bump region should recover the original
  // slope/intercept (in the non-robust fit).
  PsdResult powlaw_nr;
  for (int i = 1; i <= 40; ++i) {
    const double f = static_cast<double>(i);
    powlaw_nr.freqs_hz.push_back(f);
    powlaw_nr.psd.push_back(1.0 / f); // k=1
  }
  PsdResult powlaw_bump2 = powlaw_nr;
  for (size_t i = 0; i < powlaw_bump2.freqs_hz.size(); ++i) {
    const double f = powlaw_bump2.freqs_hz[i];
    if (f >= 8.0 && f <= 12.0) {
      powlaw_bump2.psd[i] *= 1e6; // +60 dB bump
    }
  }

  const SpectralLogLogFit fit_base_nr = spectral_loglog_fit(powlaw_nr, 1.0, 40.0, /*robust=*/false);
  const SpectralLogLogFit fit_bump_nr = spectral_loglog_fit(powlaw_bump2, 1.0, 40.0, /*robust=*/false);

  std::vector<FrequencyRange> excl;
  FrequencyRange ex;
  ex.fmin_hz = 8.0;
  ex.fmax_hz = 12.0;
  excl.push_back(ex);
  const SpectralLogLogFit fit_bump_excl = spectral_loglog_fit(powlaw_bump2, 1.0, 40.0, excl, /*robust=*/false);

  // Base fit is exact: log10(1/f) = -log10(f).
  assert(approx(fit_base_nr.slope, -1.0, 1e-12));
  assert(approx(fit_base_nr.intercept, 0.0, 1e-12));

  // The bump should perturb the non-robust fit substantially.
  assert(std::fabs(fit_bump_nr.slope - fit_base_nr.slope) > 0.05);

  // Excluding the bump region should recover the base fit (to numerical precision).
  assert(approx(fit_bump_excl.slope, fit_base_nr.slope, 1e-9));
  assert(approx(fit_bump_excl.intercept, fit_base_nr.intercept, 1e-9));

  // Case 7: two-slope aperiodic fit with knee recovery.
  // Build a continuous piecewise 1/f^k spectrum with a knee at 10 Hz:
  //   f <= 10: P = 1 / f^1
  //   f >  10: P = 10 / f^2  (chosen so the spectrum is continuous at 10 Hz)
  PsdResult powlaw_2s;
  for (int i = 1; i <= 40; ++i) {
    const double f = static_cast<double>(i);
    powlaw_2s.freqs_hz.push_back(f);
    if (f <= 10.0) {
      powlaw_2s.psd.push_back(1.0 / f);
    } else {
      powlaw_2s.psd.push_back(10.0 / (f * f));
    }
  }

  const SpectralLogLogTwoSlopeFit fit2s = spectral_loglog_two_slope_fit(powlaw_2s,
                                                                      1.0,
                                                                      40.0,
                                                                      /*robust=*/false,
                                                                      /*max_iter=*/0,
                                                                      /*min_points_per_side=*/5);
  assert(fit2s.found);
  assert(approx(fit2s.knee_hz, 10.0, 1e-6));
  assert(approx(fit2s.slope_low, -1.0, 1e-6));
  assert(approx(fit2s.slope_high, -2.0, 1e-6));
  assert(std::isfinite(fit2s.rmse));
  assert(fit2s.rmse < 1e-10);
  assert(std::isfinite(fit2s.rmse_unweighted));
  assert(fit2s.rmse_unweighted < 1e-10);


  // Case 8: aperiodic knee (curved) fit recovery.
  // Model: log10(P(f)) = offset - log10(knee + f^exponent)
  // Choose offset=1.0, exponent=2.0, knee_freq=5 Hz => knee = 25.
  const double knee_offset = 1.0;
  const double knee_exponent = 2.0;
  const double knee_freq = 5.0;
  const double knee_param = std::pow(knee_freq, knee_exponent);

  PsdResult knee_psd;
  for (int i = 1; i <= 40; ++i) {
    const double f = static_cast<double>(i);
    knee_psd.freqs_hz.push_back(f);
    const double p = std::pow(10.0, knee_offset) / (knee_param + std::pow(f, knee_exponent));
    knee_psd.psd.push_back(p);
  }

  const SpectralAperiodicKneeFit knee_fit = spectral_aperiodic_knee_fit(knee_psd, 1.0, 40.0, /*robust=*/false, /*max_iter=*/0);
  assert(knee_fit.found);
  assert(std::isfinite(knee_fit.offset));
  assert(std::isfinite(knee_fit.exponent));
  assert(std::isfinite(knee_fit.knee_freq_hz));
  assert(std::isfinite(knee_fit.knee));

  // Grid-search based fit: allow small tolerance.
  assert(std::fabs(knee_fit.offset - knee_offset) < 0.05);
  assert(std::fabs(knee_fit.exponent - knee_exponent) < 0.08);
  assert(std::fabs(knee_fit.knee_freq_hz - knee_freq) < 0.4);
  assert(knee_fit.r2 > 0.999);
  assert(knee_fit.rmse < 2e-3);
  assert(std::isfinite(knee_fit.rmse_unweighted));
  assert(knee_fit.rmse_unweighted < 2e-3);


  // Case 9: prominence / periodic residual metrics relative to non-loglog aperiodic backgrounds.
  // Build a simple PSD with a constant aperiodic background = 1 and a symmetric "bump":
  // freqs: 1,2,3,4   PSD: 1,2,2,1  => periodic residual = 0,1,1,0.
  PsdResult simple;
  simple.freqs_hz = {1.0, 2.0, 3.0, 4.0};
  simple.psd = {1.0, 2.0, 2.0, 1.0};

  // Two-slope background: constant 1 => log10(background)=0 everywhere.
  SpectralLogLogTwoSlopeFit bg2s{};
  bg2s.found = true;
  bg2s.knee_hz = 3.0;
  bg2s.slope_low = 0.0;
  bg2s.intercept_low = 0.0;
  bg2s.slope_high = 0.0;
  bg2s.intercept_high = 0.0;

  const double ppow_2s = spectral_periodic_power_from_two_slope_fit(simple, 1.0, 4.0, bg2s, /*positive_only=*/true);
  assert(approx(ppow_2s, 2.0, 1e-12));

  const double pfrac_2s =
      spectral_periodic_power_fraction_from_two_slope_fit(simple, 1.0, 4.0, bg2s, /*positive_only=*/true);
  // Total power in [1,4]: trapezoid area = 5.0 => 2/5 = 0.4
  assert(approx(pfrac_2s, 0.4, 1e-12));

  const double sef50_2s = spectral_periodic_edge_frequency_from_two_slope_fit(simple, 1.0, 4.0, bg2s, /*edge=*/0.5);
  assert(approx(sef50_2s, 2.5, 1e-12));

  const double prom2_2s = spectral_prominence_db_from_two_slope_fit(simple, 2.0, bg2s);
  assert(approx(prom2_2s, 10.0 * std::log10(2.0), 1e-12));

  // Knee-model background: choose parameters so background is constant 1.
  // log10(P(f)) = offset - log10(knee + f^exponent)
  // Set offset=0, knee=0, exponent=0 => log10(P)=0 - log10(1)=0.
  SpectralAperiodicKneeFit bgk{};
  bgk.found = true;
  bgk.offset = 0.0;
  bgk.exponent = 0.0;
  bgk.knee = 0.0;
  bgk.knee_freq_hz = 0.0;

  const double ppow_k = spectral_periodic_power_from_knee_fit(simple, 1.0, 4.0, bgk, /*positive_only=*/true);
  assert(approx(ppow_k, 2.0, 1e-12));
  const double sef50_k =
      spectral_periodic_edge_frequency_from_knee_fit(simple, 1.0, 4.0, bgk, /*edge=*/0.5);
  assert(approx(sef50_k, 2.5, 1e-12));
  const double prom2_k = spectral_prominence_db_from_knee_fit(simple, 2.0, bgk);
  assert(approx(prom2_k, 10.0 * std::log10(2.0), 1e-12));

  // Case 10: evaluate the two-slope aperiodic background in log10 domain.
  // Use the piecewise spectrum from Case 7.
  SpectralLogLogTwoSlopeFit bg2s_eval{};
  bg2s_eval.found = true;
  bg2s_eval.knee_hz = 10.0;
  bg2s_eval.slope_low = -1.0;
  bg2s_eval.intercept_low = 0.0;
  bg2s_eval.slope_high = -2.0;
  bg2s_eval.intercept_high = 1.0; // continuity at 10 Hz

  const double yhat_5 = spectral_aperiodic_log10_psd_from_two_slope_fit(bg2s_eval, 5.0);
  assert(approx(yhat_5, -std::log10(5.0), 1e-12));

  const double yhat_20 = spectral_aperiodic_log10_psd_from_two_slope_fit(bg2s_eval, 20.0);
  assert(approx(yhat_20, 1.0 - 2.0 * std::log10(20.0), 1e-12));

  // Knee-model log10 evaluation sanity check.
  const double yhat_knee_10 = spectral_aperiodic_log10_psd_from_knee_fit(bgk, 10.0);
  assert(approx(yhat_knee_10, 0.0, 1e-12));


  std::cout << "test_spectral_features OK\n";
  return 0;
}
