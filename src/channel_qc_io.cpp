#include "qeeg/channel_qc_io.hpp"

#include "qeeg/utils.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace qeeg {
namespace {

static bool is_comment_or_empty(const std::string& line) {
  const std::string t = trim(line);
  return t.empty() || (!t.empty() && t[0] == '#');
}

static int find_col(const std::vector<std::string>& header, const std::vector<std::string>& want) {
  for (size_t i = 0; i < header.size(); ++i) {
    const std::string h = to_lower(trim(header[i]));
    for (const auto& w : want) {
      if (h == w) return static_cast<int>(i);
    }
  }
  return -1;
}

static bool parse_bool_token(const std::string& s, bool* out) {
  if (!out) return false;
  const std::string t = to_lower(trim(s));
  if (t.empty() || t == "n/a" || t == "na") {
    *out = false;
    return true;
  }
  if (t == "1" || t == "true" || t == "yes" || t == "y") {
    *out = true;
    return true;
  }
  if (t == "0" || t == "false" || t == "no" || t == "n") {
    *out = false;
    return true;
  }
  // Try integer parsing as a fallback.
  try {
    size_t pos = 0;
    const int v = std::stoi(t, &pos, 10);
    if (pos != t.size()) return false;
    *out = (v != 0);
    return true;
  } catch (...) {
    return false;
  }
}

static std::filesystem::path normalize_qc_path(const std::string& path) {
  if (path.empty()) return {};
  std::error_code ec;
  const auto p = std::filesystem::u8path(path);
  if (std::filesystem::is_regular_file(p, ec)) {
    // If the user passes a file inside an outdir (e.g. qc_summary.txt), treat
    // its parent directory as the outdir and look for channel_qc.csv within it.
    const std::string ext = to_lower(p.extension().u8string());
    if (ext == ".csv" || ext == ".txt") {
      return p;
    }
    return p.parent_path();
  }
  return p;
}

} // namespace

ChannelQcMap load_channel_qc_csv(const std::string& path) {
  if (path.empty()) throw std::runtime_error("channel_qc: empty path");

  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("channel_qc: failed to open: " + path);

  std::string line;
  bool have_header = false;
  std::vector<std::string> header;
  int col_channel = -1;
  int col_bad = -1;
  int col_reasons = -1;

  ChannelQcMap out;
  out.reserve(128);

  while (std::getline(f, line)) {
    if (!have_header) {
      line = strip_utf8_bom(line);
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (is_comment_or_empty(line)) continue;

    if (!have_header) {
      header = split_csv_row(line, ',');
      col_channel = find_col(header, {"channel", "name"});
      col_bad = find_col(header, {"bad"});
      col_reasons = find_col(header, {"reasons", "reason"});
      if (col_channel < 0) throw std::runtime_error("channel_qc.csv missing required column: channel");
      if (col_bad < 0) throw std::runtime_error("channel_qc.csv missing required column: bad");
      have_header = true;
      continue;
    }

    const auto row = split_csv_row(line, ',');
    if (static_cast<size_t>(col_channel) >= row.size() || static_cast<size_t>(col_bad) >= row.size()) {
      continue;
    }

    const std::string ch_raw = trim(row[static_cast<size_t>(col_channel)]);
    if (ch_raw.empty()) continue;
    const std::string key = normalize_channel_name(ch_raw);
    if (key.empty()) continue;

    bool bad = false;
    if (!parse_bool_token(row[static_cast<size_t>(col_bad)], &bad)) {
      throw std::runtime_error("channel_qc.csv: failed to parse bad flag for channel: " + ch_raw);
    }

    std::string reasons;
    if (col_reasons >= 0 && static_cast<size_t>(col_reasons) < row.size()) {
      reasons = trim(row[static_cast<size_t>(col_reasons)]);
    }

    ChannelQcLabel lab;
    lab.bad = bad;
    lab.reasons = reasons;
    lab.name = ch_raw;
    out[key] = lab;
  }

  if (!have_header) {
    throw std::runtime_error("channel_qc.csv: missing header row: " + path);
  }

  return out;
}

std::vector<std::string> load_channel_qc_csv_channel_names(const std::string& path) {
  if (path.empty()) throw std::runtime_error("channel_qc: empty path");

  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("channel_qc: failed to open: " + path);

  std::string line;
  bool have_header = false;
  char delim = ',';
  std::vector<std::string> header;
  int col_channel = -1;

  std::vector<std::string> out;
  out.reserve(128);
  std::unordered_set<std::string> seen_norm;
  seen_norm.reserve(128);

  while (std::getline(f, line)) {
    if (!have_header) {
      line = strip_utf8_bom(line);
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (is_comment_or_empty(line)) continue;

    if (!have_header) {
      const std::string t = trim(line);
      delim = (t.find('\t') != std::string::npos) ? '\t' : ',';
      header = (delim == '\t') ? split(t, delim) : split_csv_row(t, delim);
      col_channel = find_col(header, {"channel", "name"});
      if (col_channel < 0) throw std::runtime_error("channel_qc.csv missing required column: channel");
      have_header = true;
      continue;
    }

    const auto row = (delim == '\t') ? split(line, delim) : split_csv_row(line, delim);
    if (static_cast<size_t>(col_channel) >= row.size()) continue;
    const std::string ch_raw = trim(row[static_cast<size_t>(col_channel)]);
    if (ch_raw.empty()) continue;

    const std::string key = normalize_channel_name(ch_raw);
    if (key.empty()) continue;
    if (!seen_norm.insert(key).second) {
      throw std::runtime_error("channel_qc.csv: duplicate channel name (after normalization): " + ch_raw);
    }

    out.push_back(ch_raw);
  }

  if (!have_header) {
    throw std::runtime_error("channel_qc.csv: missing header row: " + path);
  }

  return out;
}

ChannelQcMap load_bad_channels_list(const std::string& path) {
  if (path.empty()) throw std::runtime_error("bad_channels: empty path");

  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) throw std::runtime_error("bad_channels: failed to open: " + path);

  ChannelQcMap out;
  out.reserve(64);

  std::string line;
  bool first = true;
  while (std::getline(f, line)) {
    if (first) {
      line = strip_utf8_bom(line);
      first = false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (is_comment_or_empty(line)) continue;

    const std::string ch_raw = trim(line);
    if (ch_raw.empty()) continue;
    const std::string key = normalize_channel_name(ch_raw);
    if (key.empty()) continue;
    ChannelQcLabel lab;
    lab.bad = true;
    lab.reasons.clear();
    lab.name = ch_raw;
    out[key] = lab;
  }

  return out;
}

ChannelQcMap load_channel_qc_any(const std::string& path, std::string* resolved_path) {
  if (path.empty()) throw std::runtime_error("channel_qc: empty path");

  std::error_code ec;
  std::filesystem::path p = normalize_qc_path(path);
  if (p.empty()) throw std::runtime_error("channel_qc: empty path");

  if (std::filesystem::is_directory(p, ec)) {
    const auto csv = p / "channel_qc.csv";
    const auto txt = p / "bad_channels.txt";
    if (std::filesystem::is_regular_file(csv, ec)) {
      if (resolved_path) *resolved_path = csv.u8string();
      return load_channel_qc_csv(csv.u8string());
    }
    if (std::filesystem::is_regular_file(txt, ec)) {
      if (resolved_path) *resolved_path = txt.u8string();
      return load_bad_channels_list(txt.u8string());
    }
    throw std::runtime_error("channel_qc: no channel_qc.csv or bad_channels.txt found in: " + p.u8string());
  }

  if (std::filesystem::is_regular_file(p, ec)) {
    const std::string ext = to_lower(p.extension().u8string());
    if (ext == ".csv") {
      if (resolved_path) *resolved_path = p.u8string();
      return load_channel_qc_csv(p.u8string());
    }
    // Treat any non-.csv file as a simple bad-channels list.
    if (resolved_path) *resolved_path = p.u8string();
    return load_bad_channels_list(p.u8string());
  }

  throw std::runtime_error("channel_qc: path not found: " + path);
}

} // namespace qeeg
