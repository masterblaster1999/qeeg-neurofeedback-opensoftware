#include "qeeg/connectivity_graph.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace qeeg;

static const ConnectivityNodeMetrics* find_node(const std::vector<ConnectivityNodeMetrics>& nodes,
                                                const std::string& name) {
  for (const auto& n : nodes) {
    if (n.node == name) return &n;
  }
  return nullptr;
}

static const ConnectivityRegionPairMetrics* find_region_pair(const std::vector<ConnectivityRegionPairMetrics>& pairs,
                                                             const std::string& a,
                                                             const std::string& b) {
  for (const auto& p : pairs) {
    if ((p.region_a == a && p.region_b == b) || (p.region_a == b && p.region_b == a)) return &p;
  }
  return nullptr;
}

int main() {
  // Hemisphere inference.
  TEST_CHECK(infer_connectivity_hemisphere("F3") == ConnectivityHemisphere::Left);
  TEST_CHECK(infer_connectivity_hemisphere("F4") == ConnectivityHemisphere::Right);
  TEST_CHECK(infer_connectivity_hemisphere("Cz") == ConnectivityHemisphere::Midline);
  TEST_CHECK(infer_connectivity_hemisphere("AFz") == ConnectivityHemisphere::Midline);
  TEST_CHECK(infer_connectivity_hemisphere("Unknown") == ConnectivityHemisphere::Unknown);

  // Lobe inference.
  TEST_CHECK(infer_connectivity_lobe("Fp1") == ConnectivityLobe::Frontal);
  TEST_CHECK(infer_connectivity_lobe("AF3") == ConnectivityLobe::Frontal);
  TEST_CHECK(infer_connectivity_lobe("Cz") == ConnectivityLobe::Central);
  TEST_CHECK(infer_connectivity_lobe("P7") == ConnectivityLobe::Parietal);
  TEST_CHECK(infer_connectivity_lobe("O2") == ConnectivityLobe::Occipital);
  TEST_CHECK(infer_connectivity_lobe("T8") == ConnectivityLobe::Temporal);

  // Region label combines lobe + hemisphere short.
  TEST_CHECK(infer_connectivity_region_label("F3") == "Frontal_L");
  TEST_CHECK(infer_connectivity_region_label("F4") == "Frontal_R");
  TEST_CHECK(infer_connectivity_region_label("Cz") == "Central_Z");

  // Graph metrics.
  std::vector<ConnectivityEdge> edges;
  edges.push_back({"F3", "F4", 0.50});
  edges.push_back({"F3", "Cz", 0.20});
  edges.push_back({"Cz", "F4", 0.10});

  // Add a duplicate reverse edge; it should be de-duplicated.
  edges.push_back({"F4", "F3", 0.50});

  const ConnectivityGraphMetrics m = compute_connectivity_graph_metrics(edges);
  TEST_CHECK(m.nodes.size() == 3);

  const auto* n_f3 = find_node(m.nodes, "F3");
  const auto* n_f4 = find_node(m.nodes, "F4");
  const auto* n_cz = find_node(m.nodes, "Cz");
  TEST_CHECK(n_f3 != nullptr);
  TEST_CHECK(n_f4 != nullptr);
  TEST_CHECK(n_cz != nullptr);

  TEST_CHECK(n_f3->degree == 2);
  TEST_CHECK(n_f4->degree == 2);
  TEST_CHECK(n_cz->degree == 2);

  TEST_CHECK(std::fabs(n_f3->strength - 0.70) < 1e-9);
  TEST_CHECK(std::fabs(n_f4->strength - 0.60) < 1e-9);
  TEST_CHECK(std::fabs(n_cz->strength - 0.30) < 1e-9);

  TEST_CHECK(std::fabs(n_f3->mean_weight - 0.35) < 1e-9);
  TEST_CHECK(std::fabs(n_f4->mean_weight - 0.30) < 1e-9);
  TEST_CHECK(std::fabs(n_cz->mean_weight - 0.15) < 1e-9);

  // Region pair summary.
  const auto* p_ff = find_region_pair(m.region_pairs, "Frontal_L", "Frontal_R");
  TEST_CHECK(p_ff != nullptr);
  TEST_CHECK(p_ff->edge_count == 1);
  TEST_CHECK(std::fabs(p_ff->sum_weight - 0.50) < 1e-9);
  TEST_CHECK(std::fabs(p_ff->mean_weight - 0.50) < 1e-9);

  const auto* p_f3_cz = find_region_pair(m.region_pairs, "Frontal_L", "Central_Z");
  TEST_CHECK(p_f3_cz != nullptr);
  TEST_CHECK(p_f3_cz->edge_count == 1);
  TEST_CHECK(std::fabs(p_f3_cz->sum_weight - 0.20) < 1e-9);

  const auto* p_f4_cz = find_region_pair(m.region_pairs, "Frontal_R", "Central_Z");
  TEST_CHECK(p_f4_cz != nullptr);
  TEST_CHECK(p_f4_cz->edge_count == 1);
  TEST_CHECK(std::fabs(p_f4_cz->sum_weight - 0.10) < 1e-9);

  return 0;
}
