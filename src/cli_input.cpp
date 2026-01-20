#include "qeeg/cli_input.hpp"

#include "qeeg/run_meta.hpp"
#include "qeeg/utils.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace qeeg {

namespace {

static bool is_tabular_ext(const std::filesystem::path& p) {
  const std::string ext = to_lower(p.extension().u8string());
  return ext == ".csv" || ext == ".tsv";
}

static std::string lower_filename(const std::filesystem::path& p) {
  return to_lower(p.filename().u8string());
}

static int score_candidate(const std::filesystem::path& p,
                           const ResolveInputTableOptions& opt) {
  const std::string name = lower_filename(p);
  int score = 0;

  for (size_t i = 0; i < opt.preferred_filenames.size(); ++i) {
    if (name == to_lower(opt.preferred_filenames[i])) {
      score += 1000 - static_cast<int>(i);
      break;
    }
  }

  for (size_t i = 0; i < opt.preferred_contains.size(); ++i) {
    const std::string needle = to_lower(opt.preferred_contains[i]);
    if (!needle.empty() && name.find(needle) != std::string::npos) {
      score += 500 - static_cast<int>(i);
      break;
    }
  }

  // Gentle heuristics that help common qeeg outputs without requiring explicit
  // preferences.
  if (ends_with(name, "_pairs.csv") || ends_with(name, "_pairs.tsv")) score += 10;
  if (name.find("matrix") != std::string::npos) score += 5;

  // Prefer CSV slightly over TSV (most qeeg writers emit CSV as primary).
  if (to_lower(p.extension().u8string()) == ".csv") score += 1;

  return score;
}

static std::vector<std::filesystem::path> list_tabular_files_in_dir(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return out;

  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const std::filesystem::path p = e.path();
    if (!is_tabular_ext(p)) continue;
    out.push_back(p);
  }

  std::sort(out.begin(), out.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.u8string() < b.u8string();
            });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static std::vector<std::filesystem::path> list_run_meta_files_in_dir(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return out;

  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const std::filesystem::path p = e.path();
    const std::string name = to_lower(p.filename().u8string());
    if (ends_with(name, "_run_meta.json")) {
      out.push_back(p);
    }
  }

  std::sort(out.begin(), out.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.u8string() < b.u8string();
            });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static std::filesystem::path choose_best(const std::vector<std::filesystem::path>& candidates,
                                        const ResolveInputTableOptions& opt,
                                        bool* out_matched_preferences) {
  if (out_matched_preferences) *out_matched_preferences = false;
  if (candidates.empty()) return {};

  int best_score = std::numeric_limits<int>::min();
  std::filesystem::path best;

  for (const auto& p : candidates) {
    const int s = score_candidate(p, opt);
    if (s > best_score) {
      best_score = s;
      best = p;
    } else if (s == best_score) {
      // Stable tie-break: lexicographic by path.
      if (p.u8string() < best.u8string()) best = p;
    }
  }

  if (out_matched_preferences) {
    *out_matched_preferences = (best_score >= 500);
  }

  if (!opt.allow_any && best_score <= 0) return {};
  return best;
}

static bool is_run_meta_json(const std::filesystem::path& p) {
  const std::string name = to_lower(p.filename().u8string());
  return ends_with(name, "_run_meta.json");
}

static std::string canonical_key_best_effort(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
  if (ec) {
    c = std::filesystem::absolute(p, ec);
  }
  if (ec) {
    c = p;
  }
  return c.lexically_normal().u8string();
}

static void gather_tabular_from_run_meta_recursive(const std::filesystem::path& meta_path,
                                                   std::unordered_set<std::string>* visited,
                                                   std::vector<std::filesystem::path>* out,
                                                   int depth) {
  if (!visited || !out) return;
  if (depth > 8) return;

  const std::string key = canonical_key_best_effort(meta_path);
  if (!visited->insert(key).second) return;

  const std::vector<std::string> rels = read_run_meta_outputs(meta_path.u8string());
  if (rels.empty()) return;

  const std::filesystem::path base = meta_path.parent_path();

  for (const auto& rel : rels) {
    if (rel.empty()) continue;
    const std::filesystem::path p = base / std::filesystem::u8path(rel);
    if (!std::filesystem::exists(p)) continue;

    if (std::filesystem::is_regular_file(p)) {
      if (is_tabular_ext(p)) {
        out->push_back(p);
      } else if (is_run_meta_json(p)) {
        gather_tabular_from_run_meta_recursive(p, visited, out, depth + 1);
      }
      continue;
    }

    if (std::filesystem::is_directory(p)) {
      const auto files = list_tabular_files_in_dir(p);
      out->insert(out->end(), files.begin(), files.end());

      // Also follow any run-meta files directly in that directory.
      for (const auto& m : list_run_meta_files_in_dir(p)) {
        gather_tabular_from_run_meta_recursive(m, visited, out, depth + 1);
      }
    }
  }
}

static std::vector<std::filesystem::path> tabular_candidates_from_run_meta(const std::filesystem::path& meta_path) {
  std::vector<std::filesystem::path> out;

  std::unordered_set<std::string> visited;
  gather_tabular_from_run_meta_recursive(meta_path, &visited, &out, 0);

  std::sort(out.begin(), out.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.u8string() < b.u8string();
            });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static std::string describe_spec(const std::filesystem::path& p) {
  if (std::filesystem::is_directory(p)) return p.u8string() + "/";
  return p.u8string();
}

struct ParsedInputSpec {
  std::string path;     // the filesystem path portion
  std::string selector; // optional filename selector (after '#')
};

static ParsedInputSpec parse_input_spec_with_selector(const std::string& spec_trim) {
  ParsedInputSpec ps;

  const size_t pos = spec_trim.find('#');
  if (pos == std::string::npos) {
    ps.path = spec_trim;
    return ps;
  }

  ps.path = trim(spec_trim.substr(0, pos));
  ps.selector = trim(spec_trim.substr(pos + 1));

  if (ps.path.empty()) {
    throw std::runtime_error("Invalid input spec: missing path before '#': " + spec_trim);
  }
  if (ps.selector.empty()) {
    throw std::runtime_error("Invalid input spec: empty selector after '#': " + spec_trim);
  }

  return ps;
}

static bool has_glob_chars(const std::string& pattern) {
  return pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos;
}

// Simple glob matcher supporting '*' and '?' (case-sensitive).
//
// The caller should lower-case both pattern and text when doing
// case-insensitive matching.
static bool glob_match(const std::string& pattern, const std::string& text) {
  // Iterative wildcard matcher.
  //   '*' matches any sequence (including empty)
  //   '?' matches exactly one character
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string::npos;
  size_t match = 0;

  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
      continue;
    }

    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      match = t;
      continue;
    }

    if (star != std::string::npos) {
      p = star + 1;
      t = ++match;
      continue;
    }

    return false;
  }

  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

static bool selector_matches_name_lower(const std::string& filename_lower,
                                       const std::string& selector_raw) {
  if (selector_raw.empty()) return true;

  const std::string sel = to_lower(selector_raw);
  if (sel.empty()) return true;

  if (has_glob_chars(sel)) {
    return glob_match(sel, filename_lower);
  }

  if (filename_lower == sel) return true;
  if (filename_lower.find(sel) != std::string::npos) return true;
  return false;
}

static bool matches_selector(const std::filesystem::path& p,
                             const std::string& selector_raw) {
  if (selector_raw.empty()) return true;
  return selector_matches_name_lower(lower_filename(p), selector_raw);
}

static std::vector<std::filesystem::path> filter_by_selector(const std::vector<std::filesystem::path>& cand,
                                                             const std::string& selector_raw) {
  if (selector_raw.empty()) return cand;

  std::vector<std::filesystem::path> out;
  out.reserve(cand.size());
  for (const auto& p : cand) {
    if (matches_selector(p, selector_raw)) out.push_back(p);
  }
  return out;
}

static std::string selector_note_suffix(const std::string& selector_raw) {
  if (selector_raw.empty()) return std::string();
  return " (selector: " + selector_raw + ")";
}

static bool is_allowed_ext(const std::filesystem::path& p,
                           const std::vector<std::string>& allowed_extensions) {
  const std::string ext = to_lower(p.extension().u8string());
  for (const auto& a : allowed_extensions) {
    if (ext == to_lower(a)) return true;
  }
  return false;
}

static int ext_rank_score(const std::string& ext,
                          const ResolveInputFileOptions& opt) {
  for (size_t i = 0; i < opt.allowed_extensions.size(); ++i) {
    if (ext == to_lower(opt.allowed_extensions[i])) {
      // Earlier extensions are preferred.
      return 200 - static_cast<int>(i);
    }
  }
  return 0;
}

static bool is_ascii_like_ext(const std::string& ext) {
  return ext == ".csv" || ext == ".tsv" || ext == ".txt" || ext == ".asc" || ext == ".ascii";
}

static int score_candidate_file(const std::filesystem::path& p,
                                const ResolveInputFileOptions& opt) {
  const std::string name = lower_filename(p);
  const std::string ext = to_lower(p.extension().u8string());

  int score = 0;
  score += ext_rank_score(ext, opt);

  for (size_t i = 0; i < opt.preferred_filenames.size(); ++i) {
    if (name == to_lower(opt.preferred_filenames[i])) {
      score += 1000 - static_cast<int>(i);
      break;
    }
  }

  for (size_t i = 0; i < opt.preferred_contains.size(); ++i) {
    const std::string needle = to_lower(opt.preferred_contains[i]);
    if (!needle.empty() && name.find(needle) != std::string::npos) {
      score += 500 - static_cast<int>(i);
      break;
    }
  }

  // Avoid patterns are mainly meant to prevent accidentally selecting derived
  // analysis tables when the user passes an output directory.
  if (is_ascii_like_ext(ext)) {
    for (const auto& bad : opt.avoid_contains) {
      const std::string needle = to_lower(bad);
      if (!needle.empty() && name.find(needle) != std::string::npos) {
        score -= 1500;
        break;
      }
    }
  }

  return score;
}

static std::vector<std::filesystem::path> list_files_in_dir_by_ext(const std::filesystem::path& dir,
                                                                   const ResolveInputFileOptions& opt) {
  std::vector<std::filesystem::path> out;
  if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return out;

  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const auto& p = entry.path();
    if (!is_allowed_ext(p, opt.allowed_extensions)) continue;
    out.push_back(p);
  }

  std::sort(out.begin(), out.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.u8string() < b.u8string();
            });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static void gather_files_from_run_meta_recursive(const std::filesystem::path& meta_path,
                                                   const ResolveInputFileOptions& opt,
                                                   std::unordered_set<std::string>* visited,
                                                   std::vector<std::filesystem::path>* out,
                                                   int depth) {
  if (!visited || !out) return;
  if (depth > 8) return;

  const std::string key = canonical_key_best_effort(meta_path);
  if (!visited->insert(key).second) return;

  const std::vector<std::string> rels = read_run_meta_outputs(meta_path.u8string());
  if (rels.empty()) return;

  const std::filesystem::path base = meta_path.parent_path();

  for (const auto& rel : rels) {
    if (rel.empty()) continue;
    const std::filesystem::path p = base / std::filesystem::u8path(rel);
    if (!std::filesystem::exists(p)) continue;

    if (std::filesystem::is_regular_file(p)) {
      if (is_allowed_ext(p, opt.allowed_extensions)) {
        out->push_back(p);
      } else if (is_run_meta_json(p)) {
        gather_files_from_run_meta_recursive(p, opt, visited, out, depth + 1);
      }
      continue;
    }

    if (std::filesystem::is_directory(p)) {
      const auto files = list_files_in_dir_by_ext(p, opt);
      out->insert(out->end(), files.begin(), files.end());

      // Also follow any run-meta files directly in that directory.
      for (const auto& m : list_run_meta_files_in_dir(p)) {
        gather_files_from_run_meta_recursive(m, opt, visited, out, depth + 1);
      }
    }
  }
}

static std::vector<std::filesystem::path> file_candidates_from_run_meta(const std::filesystem::path& meta_path,
                                                                        const ResolveInputFileOptions& opt) {
  std::vector<std::filesystem::path> out;

  std::unordered_set<std::string> visited;
  gather_files_from_run_meta_recursive(meta_path, opt, &visited, &out, 0);

  std::sort(out.begin(), out.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.u8string() < b.u8string();
            });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static std::filesystem::path choose_best_file(const std::vector<std::filesystem::path>& cand,
                                              const ResolveInputFileOptions& opt,
                                              bool* out_matched_preferences) {
  if (out_matched_preferences) *out_matched_preferences = false;
  if (cand.empty()) return {};

  std::filesystem::path best;
  int best_score = std::numeric_limits<int>::min();

  for (const auto& p : cand) {
    const int s = score_candidate_file(p, opt);
    if (s > best_score) {
      best_score = s;
      best = p;
    }
  }

  if (!opt.allow_any && best_score <= 0) {
    return {};
  }

  if (out_matched_preferences) {
    *out_matched_preferences = (best_score >= 500);
  }

  return best;
}

} // namespace

ResolvedInputPath resolve_input_table_path(const std::string& input_spec,
                                          const ResolveInputTableOptions& opt) {
  const std::string spec_trim0 = trim(input_spec);
  if (spec_trim0.empty()) {
    throw std::runtime_error("--input is required");
  }

  const ParsedInputSpec spec = parse_input_spec_with_selector(spec_trim0);

  const std::filesystem::path p = std::filesystem::u8path(spec.path);
  if (!std::filesystem::exists(p)) {
    throw std::runtime_error("Input path does not exist: " + spec.path);
  }

  ResolvedInputPath r;

  if (std::filesystem::is_regular_file(p)) {
    const std::string ext = to_lower(p.extension().u8string());
    if (ext == ".csv" || ext == ".tsv") {
      if (!spec.selector.empty() && !matches_selector(p, spec.selector)) {
        throw std::runtime_error("Input selector '" + spec.selector + "' does not match file: " + p.u8string());
      }
      r.path = p.u8string();
      return r;
    }

    if (ext == ".json") {
      // Treat as run meta and select a tabular output.
      std::vector<std::filesystem::path> cand = filter_by_selector(tabular_candidates_from_run_meta(p), spec.selector);
      bool matched = false;
      std::filesystem::path best = choose_best(cand, opt, &matched);

      if (best.empty()) {
        // Fallback: scan the containing directory (covers cases where outputs
        // are present but not listed, or when the meta schema differs).
        const std::filesystem::path dir = p.parent_path();
        cand = filter_by_selector(list_tabular_files_in_dir(dir), spec.selector);
        best = choose_best(cand, opt, &matched);
      }

      if (best.empty()) {
        throw std::runtime_error("No .csv/.tsv outputs found for run meta: " + p.u8string() + selector_note_suffix(spec.selector));
      }

      r.path = best.u8string();
      r.note = "Resolved --input from run meta: " + p.filename().u8string() + " -> " + best.filename().u8string();
      r.note += selector_note_suffix(spec.selector);
      return r;
    }

    throw std::runtime_error("Unsupported --input file type (expected .csv/.tsv or *_run_meta.json): " + p.u8string());
  }

  if (std::filesystem::is_directory(p)) {
    std::vector<std::filesystem::path> cand;

    // 1) Direct files in the directory.
    cand = list_tabular_files_in_dir(p);

    // 2) Also consider tabular files referenced by any run meta files.
    //    This enables cases where outputs live in a subfolder.
    for (const auto& meta : list_run_meta_files_in_dir(p)) {
      const auto more = tabular_candidates_from_run_meta(meta);
      cand.insert(cand.end(), more.begin(), more.end());
    }

    std::sort(cand.begin(), cand.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                return a.u8string() < b.u8string();
              });
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

    cand = filter_by_selector(cand, spec.selector);

    bool matched = false;
    const std::filesystem::path best = choose_best(cand, opt, &matched);
    if (best.empty()) {
      throw std::runtime_error("No .csv/.tsv outputs found in directory: " + describe_spec(p) + selector_note_suffix(spec.selector));
    }

    r.path = best.u8string();
    r.note = "Resolved --input from directory: " + p.u8string() + " -> " + best.filename().u8string();
    r.note += selector_note_suffix(spec.selector);
    return r;
  }

  throw std::runtime_error("Unsupported --input path: " + spec.path);
}


ResolvedInputPath resolve_input_file_path(const std::string& input_spec,
                                         const ResolveInputFileOptions& opt) {
  const std::string spec_trim0 = trim(input_spec);
  if (spec_trim0.empty()) {
    throw std::runtime_error("--input is required");
  }

  if (opt.allowed_extensions.empty()) {
    throw std::runtime_error("Internal error: no allowed extensions configured for resolver");
  }

  const ParsedInputSpec spec = parse_input_spec_with_selector(spec_trim0);

  const std::filesystem::path p = std::filesystem::u8path(spec.path);
  if (!std::filesystem::exists(p)) {
    throw std::runtime_error("Input path does not exist: " + spec.path);
  }

  ResolvedInputPath r;

  if (std::filesystem::is_regular_file(p)) {
    const std::string ext = to_lower(p.extension().u8string());
    if (is_allowed_ext(p, opt.allowed_extensions)) {
      if (!spec.selector.empty() && !matches_selector(p, spec.selector)) {
        throw std::runtime_error("Input selector '" + spec.selector + "' does not match file: " + p.u8string());
      }
      r.path = p.u8string();
      return r;
    }

    if (ext == ".json") {
      // Treat as run meta and select an output with an allowed extension.
      std::vector<std::filesystem::path> cand = filter_by_selector(file_candidates_from_run_meta(p, opt), spec.selector);
      bool matched = false;
      std::filesystem::path best = choose_best_file(cand, opt, &matched);

      if (best.empty()) {
        // Fallback: scan the containing directory.
        const std::filesystem::path dir = p.parent_path();
        cand = filter_by_selector(list_files_in_dir_by_ext(dir, opt), spec.selector);
        best = choose_best_file(cand, opt, &matched);
      }

      if (best.empty()) {
        throw std::runtime_error("No compatible outputs found for run meta: " + p.u8string() + selector_note_suffix(spec.selector));
      }

      r.path = best.u8string();
      r.note = "Resolved --input from run meta: " + p.filename().u8string() + " -> " + best.filename().u8string();
      r.note += selector_note_suffix(spec.selector);
      return r;
    }

    throw std::runtime_error("Unsupported --input file type: " + p.u8string());
  }

  if (std::filesystem::is_directory(p)) {
    std::vector<std::filesystem::path> cand;

    // 1) Direct files in the directory.
    cand = list_files_in_dir_by_ext(p, opt);

    // 2) Also consider files referenced by any run meta files.
    for (const auto& meta : list_run_meta_files_in_dir(p)) {
      const auto more = file_candidates_from_run_meta(meta, opt);
      cand.insert(cand.end(), more.begin(), more.end());
    }

    std::sort(cand.begin(), cand.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                return a.u8string() < b.u8string();
              });
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

    cand = filter_by_selector(cand, spec.selector);

    bool matched = false;
    const std::filesystem::path best = choose_best_file(cand, opt, &matched);
    if (best.empty()) {
      throw std::runtime_error("No compatible outputs found in directory: " + describe_spec(p) + selector_note_suffix(spec.selector));
    }

    r.path = best.u8string();
    r.note = "Resolved --input from directory: " + p.u8string() + " -> " + best.filename().u8string();
    r.note += selector_note_suffix(spec.selector);
    return r;
  }

  throw std::runtime_error("Unsupported --input path: " + spec.path);
}

ResolvedInputPath resolve_input_recording_path(const std::string& input_spec) {
  ResolveInputFileOptions opt;
  // Match qeeg::read_recording_auto extensions.
  opt.allowed_extensions = {
      ".edf", ".edf+", ".rec",
      ".bdf", ".bdf+",
      ".vhdr",
      ".csv", ".tsv", ".txt", ".asc", ".ascii",
  };

  // Prefer common naming conventions when multiple candidates exist.
  opt.preferred_filenames = {
      "preprocessed.edf", "preprocessed.bdf", "preprocessed.vhdr", "preprocessed.csv",
      "clean.edf", "clean.bdf", "clean.vhdr", "clean.csv",
      "recording.edf", "recording.bdf", "recording.vhdr", "recording.csv",
  };

  opt.preferred_contains = {"preprocess", "preprocessed", "clean", "recording", "eeg"};

  // Avoid accidentally selecting derived outputs when the user passes an outdir.
  opt.avoid_contains = {
      "bandpower", "bandpowers", "bandratios",
      "coherence", "imcoh", "plv", "pac",
      "spectrogram", "spectrum", "psd",
      "topomap", "region", "summary", "connectivity",
      "qc", "quality", "segments", "events", "annotations",
      "run_meta", "derived",
  };

  opt.allow_any = false;

  return resolve_input_file_path(input_spec, opt);
}

} // namespace qeeg
