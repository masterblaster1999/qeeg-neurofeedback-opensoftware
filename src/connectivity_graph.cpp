#include "qeeg/connectivity_graph.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qeeg {

namespace {

static std::string hemisphere_short(ConnectivityHemisphere h) {
  switch (h) {
    case ConnectivityHemisphere::Left: return "L";
    case ConnectivityHemisphere::Right: return "R";
    case ConnectivityHemisphere::Midline: return "Z";
    default: return "U";
  }
}

static std::string normalize_for_region(const std::string& ch) {
  // normalize_channel_name does several useful cleanups (strip -REF, map
  // legacy aliases like T3->T7, ...) but returns a canonical label in a
  // human-friendly case. For our lightweight prefix matching, we just
  // lowercase it.
  return to_lower(normalize_channel_name(ch));
}

static ConnectivityHemisphere hemisphere_of_norm(const std::string& norm_ch) {
  // norm_ch is expected to be lowercase.
  if (norm_ch.empty()) return ConnectivityHemisphere::Unknown;
  if (ends_with(norm_ch, "z")) return ConnectivityHemisphere::Midline;

  // Find a trailing integer (10-20 / 10-10 style).
  int value = 0;
  int place = 1;
  bool saw_digit = false;
  for (int i = static_cast<int>(norm_ch.size()) - 1; i >= 0; --i) {
    const unsigned char c = static_cast<unsigned char>(norm_ch[static_cast<size_t>(i)]);
    if (std::isdigit(c) == 0) break;
    saw_digit = true;
    value += (static_cast<int>(c - '0')) * place;
    place *= 10;
  }
  if (!saw_digit) return ConnectivityHemisphere::Unknown;
  return (value % 2) ? ConnectivityHemisphere::Left : ConnectivityHemisphere::Right;
}

static ConnectivityLobe lobe_of_norm(const std::string& norm_ch) {
  // Very lightweight heuristics, intended for common 10-20 / 10-10 labels.
  //
  // Order matters: handle two-letter prefixes before single-letter buckets.
  if (norm_ch.empty()) return ConnectivityLobe::Other;

  if (starts_with(norm_ch, "fp")) return ConnectivityLobe::Frontal;
  if (starts_with(norm_ch, "af")) return ConnectivityLobe::Frontal;
  if (starts_with(norm_ch, "ft")) return ConnectivityLobe::Temporal;  // fronto-temporal
  if (starts_with(norm_ch, "tp")) return ConnectivityLobe::Temporal;  // temporo-parietal
  if (starts_with(norm_ch, "po")) return ConnectivityLobe::Occipital; // parieto-occipital
  if (starts_with(norm_ch, "fc")) return ConnectivityLobe::Central;   // fronto-central
  if (starts_with(norm_ch, "cp")) return ConnectivityLobe::Parietal;  // centro-parietal

  // Single-letter prefixes
  if (starts_with(norm_ch, "f")) return ConnectivityLobe::Frontal;
  if (starts_with(norm_ch, "c")) return ConnectivityLobe::Central;
  if (starts_with(norm_ch, "p")) return ConnectivityLobe::Parietal;
  if (starts_with(norm_ch, "o")) return ConnectivityLobe::Occipital;
  if (starts_with(norm_ch, "t")) return ConnectivityLobe::Temporal;

  return ConnectivityLobe::Other;
}

struct PairHash {
  std::size_t operator()(const std::pair<std::string, std::string>& p) const noexcept {
    const std::size_t h1 = std::hash<std::string>{}(p.first);
    const std::size_t h2 = std::hash<std::string>{}(p.second);
    // hash_combine (boost-inspired)
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};

static std::pair<std::string, std::string> canonical_pair(std::string a, std::string b) {
  if (b < a) std::swap(a, b);
  return {std::move(a), std::move(b)};
}

} // namespace

ConnectivityHemisphere infer_connectivity_hemisphere(const std::string& channel) {
  return hemisphere_of_norm(normalize_for_region(channel));
}

ConnectivityLobe infer_connectivity_lobe(const std::string& channel) {
  return lobe_of_norm(normalize_for_region(channel));
}

std::string connectivity_hemisphere_name(ConnectivityHemisphere h) {
  switch (h) {
    case ConnectivityHemisphere::Left: return "Left";
    case ConnectivityHemisphere::Right: return "Right";
    case ConnectivityHemisphere::Midline: return "Midline";
    default: return "Unknown";
  }
}

std::string connectivity_lobe_name(ConnectivityLobe l) {
  switch (l) {
    case ConnectivityLobe::Frontal: return "Frontal";
    case ConnectivityLobe::Central: return "Central";
    case ConnectivityLobe::Parietal: return "Parietal";
    case ConnectivityLobe::Occipital: return "Occipital";
    case ConnectivityLobe::Temporal: return "Temporal";
    default: return "Other";
  }
}

std::string infer_connectivity_region_label(const std::string& channel) {
  const std::string norm = normalize_for_region(channel);
  const ConnectivityHemisphere h = hemisphere_of_norm(norm);
  const ConnectivityLobe l = lobe_of_norm(norm);
  return connectivity_lobe_name(l) + "_" + hemisphere_short(h);
}

ConnectivityGraphMetrics compute_connectivity_graph_metrics(const std::vector<ConnectivityEdge>& edges) {
  struct NodeAgg {
    std::unordered_set<std::string> neighbors;
    double strength{0.0};
    double max_weight{-std::numeric_limits<double>::infinity()};
  };

  struct RegionAgg {
    std::size_t edge_count{0};
    double sum_weight{0.0};
  };

  // De-duplicate undirected pairs.
  std::unordered_set<std::pair<std::string, std::string>, PairHash> seen_edges;
  seen_edges.reserve(edges.size() * 2);

  std::unordered_map<std::string, NodeAgg> nodes;
  nodes.reserve(edges.size() * 2);

  std::unordered_map<std::pair<std::string, std::string>, RegionAgg, PairHash> region_pairs;
  region_pairs.reserve(edges.size() * 2);

  for (const auto& e : edges) {
    if (e.a.empty() || e.b.empty()) continue;
    if (e.a == e.b) continue;
    if (!std::isfinite(e.w)) continue;

    const auto key = canonical_pair(e.a, e.b);
    if (!seen_edges.insert(key).second) continue;

    // Node metrics.
    {
      NodeAgg& na = nodes[e.a];
      na.neighbors.insert(e.b);
      na.strength += e.w;
      na.max_weight = std::max(na.max_weight, e.w);
    }
    {
      NodeAgg& nb = nodes[e.b];
      nb.neighbors.insert(e.a);
      nb.strength += e.w;
      nb.max_weight = std::max(nb.max_weight, e.w);
    }

    // Coarse region summary.
    const std::string ra = infer_connectivity_region_label(e.a);
    const std::string rb = infer_connectivity_region_label(e.b);
    const auto rkey = canonical_pair(ra, rb);
    RegionAgg& rg = region_pairs[rkey];
    rg.edge_count += 1;
    rg.sum_weight += e.w;
  }

  ConnectivityGraphMetrics out;

  // Materialize node metrics in a stable order.
  {
    std::vector<std::string> names;
    names.reserve(nodes.size());
    for (const auto& kv : nodes) names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    out.nodes.reserve(names.size());
    for (const auto& n : names) {
      const auto it = nodes.find(n);
      if (it == nodes.end()) continue;
      const NodeAgg& agg = it->second;
      ConnectivityNodeMetrics m;
      m.node = n;
      m.hemisphere = infer_connectivity_hemisphere(n);
      m.lobe = infer_connectivity_lobe(n);
      m.region = infer_connectivity_region_label(n);
      m.degree = agg.neighbors.size();
      m.strength = agg.strength;
      m.mean_weight = (m.degree > 0) ? (m.strength / static_cast<double>(m.degree))
                                    : std::numeric_limits<double>::quiet_NaN();
      m.max_weight = std::isfinite(agg.max_weight) ? agg.max_weight : std::numeric_limits<double>::quiet_NaN();
      out.nodes.push_back(std::move(m));
    }
  }

  // Materialize region-pair metrics.
  {
    out.region_pairs.reserve(region_pairs.size());
    for (const auto& kv : region_pairs) {
      ConnectivityRegionPairMetrics m;
      m.region_a = kv.first.first;
      m.region_b = kv.first.second;
      m.edge_count = kv.second.edge_count;
      m.sum_weight = kv.second.sum_weight;
      m.mean_weight = (m.edge_count > 0) ? (m.sum_weight / static_cast<double>(m.edge_count))
                                         : std::numeric_limits<double>::quiet_NaN();
      out.region_pairs.push_back(std::move(m));
    }
    std::sort(out.region_pairs.begin(), out.region_pairs.end(),
              [](const ConnectivityRegionPairMetrics& x, const ConnectivityRegionPairMetrics& y) {
                if (x.region_a != y.region_a) return x.region_a < y.region_a;
                return x.region_b < y.region_b;
              });
  }

  return out;
}

} // namespace qeeg
