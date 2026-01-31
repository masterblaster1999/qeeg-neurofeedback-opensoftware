#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace qeeg {

// Lightweight graph/summary helpers for connectivity edge lists.
//
// These utilities are used by connectivity visualization/reporting code to
// provide per-node and per-region summaries.

struct ConnectivityEdge {
  std::string a;
  std::string b;
  double w{0.0};
};

enum class ConnectivityHemisphere {
  Left,
  Right,
  Midline,
  Unknown,
};

enum class ConnectivityLobe {
  Frontal,
  Central,
  Parietal,
  Occipital,
  Temporal,
  Other,
};

// Infer hemisphere from a channel label using a conservative 10-20/10-10 rule:
//  - trailing 'z' => midline
//  - trailing digits: odd => left, even => right
//  - otherwise => unknown
ConnectivityHemisphere infer_connectivity_hemisphere(const std::string& channel);

// Infer a coarse lobe bucket from a normalized channel label.
// Uses lightweight prefix heuristics (fp/af/f => frontal, c/fc => central, etc.).
ConnectivityLobe infer_connectivity_lobe(const std::string& channel);

std::string connectivity_hemisphere_name(ConnectivityHemisphere h);
std::string connectivity_lobe_name(ConnectivityLobe l);

// Region label combining lobe + hemisphere short code.
// Examples: "Frontal_L", "Central_Z", "Other_U".
std::string infer_connectivity_region_label(const std::string& channel);

struct ConnectivityNodeMetrics {
  std::string node;
  ConnectivityLobe lobe{ConnectivityLobe::Other};
  ConnectivityHemisphere hemisphere{ConnectivityHemisphere::Unknown};
  std::string region;

  std::size_t degree{0};
  double strength{0.0};
  double mean_weight{0.0};
  double max_weight{0.0};
};

struct ConnectivityRegionPairMetrics {
  std::string region_a;
  std::string region_b;
  std::size_t edge_count{0};
  double sum_weight{0.0};
  double mean_weight{0.0};
};

struct ConnectivityGraphMetrics {
  std::vector<ConnectivityNodeMetrics> nodes;
  std::vector<ConnectivityRegionPairMetrics> region_pairs;
};

// Compute per-node metrics (degree/strength) and a coarse region-to-region
// summary table from an undirected edge list.
//
// Notes:
// - Duplicate edges (A,B) and (B,A) are de-duplicated.
// - Self-loops and non-finite weights are ignored.
ConnectivityGraphMetrics compute_connectivity_graph_metrics(const std::vector<ConnectivityEdge>& edges);

} // namespace qeeg
