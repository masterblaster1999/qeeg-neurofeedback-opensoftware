#include "qeeg/channel_map.hpp"

#include "qeeg/utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace qeeg {

namespace {

static bool is_comment_or_empty_line(const std::string& s) {
  const std::string t = trim(s);
  if (t.empty()) return true;
  if (!t.empty() && t[0] == '#') return true;
  return false;
}

static std::string strip_quotes(const std::string& s) {
  std::string t = trim(s);
  if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
    t = t.substr(1, t.size() - 2);
  }
  return trim(t);
}

static bool is_drop_value(std::string v) {
  v = to_lower(trim(v));
  return v.empty() || v == "drop" || v == "none" || v == "null";
}

static size_t count_delim_outside_quotes(const std::string& line, char delim) {
  size_t n = 0;
  bool in_quotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      // Toggle quotes unless escaped by another quote
      if (i + 1 < line.size() && line[i + 1] == '"') {
        ++i; // skip escaped quote
      } else {
        in_quotes = !in_quotes;
      }
      continue;
    }
    if (!in_quotes && c == delim) ++n;
  }
  return n;
}

static char detect_delim(const std::string& header_line) {
  const size_t n_comma = count_delim_outside_quotes(header_line, ',');
  const size_t n_semi = count_delim_outside_quotes(header_line, ';');
  const size_t n_tab = count_delim_outside_quotes(header_line, '\t');

  // Prefer the delimiter that yields the most columns.
  if (n_tab >= n_comma && n_tab >= n_semi && n_tab > 0) return '\t';
  if (n_semi >= n_comma && n_semi > 0) return ';';
  return ','; // default
}

static std::vector<std::string> split_row(const std::string& line, char delim) {
  std::vector<std::string> out;
  std::string cur;
  bool in_quotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
        // Escaped quote inside quoted field
        cur.push_back('"');
        ++i;
      } else {
        in_quotes = !in_quotes;
      }
      continue;
    }
    if (!in_quotes && c == delim) {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

static bool looks_like_header_row(const std::string& a, const std::string& b) {
  const std::string al = to_lower(trim(a));
  const std::string bl = to_lower(trim(b));

  const bool left_ok =
      (al == "old" || al == "from" || al == "source" || al == "input" || al == "channel" || al == "name");
  const bool right_ok =
      (bl == "new" || bl == "to" || bl == "target" || bl == "output" || bl == "rename" || bl == "mapped");
  return left_ok && right_ok;
}

} // namespace

ChannelMap load_channel_map_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("Failed to open channel map: " + path);

  ChannelMap m;

  std::string line;
  char delim = ',';
  bool delim_known = false;

  while (std::getline(f, line)) {
    if (is_comment_or_empty_line(line)) continue;

    // Determine delimiter from the first meaningful line unless it's an "old=new" mapping.
    if (!delim_known) {
      if (line.find('=') != std::string::npos &&
          line.find(',') == std::string::npos &&
          line.find(';') == std::string::npos &&
          line.find('\t') == std::string::npos) {
        delim_known = true; // treated as key=value
      } else {
        delim = detect_delim(line);
        delim_known = true;
      }
    }

    std::string left;
    std::string right;

    const size_t eq = line.find('=');
    if (eq != std::string::npos &&
        line.find(',') == std::string::npos &&
        line.find(';') == std::string::npos &&
        line.find('\t') == std::string::npos) {
      left = strip_quotes(line.substr(0, eq));
      right = strip_quotes(line.substr(eq + 1));
    } else {
      auto cols = split_row(line, delim);
      if (cols.size() < 2) continue;
      left = strip_quotes(cols[0]);
      right = strip_quotes(cols[1]);
      if (looks_like_header_row(left, right)) continue;
    }

    const std::string key = normalize_channel_name(left);
    if (key.empty()) continue;

    m.normalized_to_name[key] = right;
  }

  return m;
}

void apply_channel_map(EEGRecording* rec, const ChannelMap& map) {
  if (!rec) throw std::runtime_error("apply_channel_map: rec is null");

  if (rec->channel_names.size() != rec->data.size()) {
    throw std::runtime_error("apply_channel_map: channel_names/data size mismatch");
  }

  std::vector<std::string> new_names;
  std::vector<std::vector<float>> new_data;
  new_names.reserve(rec->n_channels());
  new_data.reserve(rec->n_channels());

  std::unordered_set<std::string> used_norm;

  for (size_t i = 0; i < rec->n_channels(); ++i) {
    const std::string orig_name = rec->channel_names[i];
    const std::string orig_key = normalize_channel_name(orig_name);

    std::string out_name = orig_name;

    const auto it = map.normalized_to_name.find(orig_key);
    if (it != map.normalized_to_name.end()) {
      const std::string mapped = trim(it->second);
      if (is_drop_value(mapped)) {
        continue; // drop channel
      }
      out_name = mapped;
    }

    out_name = trim(out_name);
    if (out_name.empty()) continue;

    const std::string out_key = normalize_channel_name(out_name);
    if (out_key.empty()) {
      throw std::runtime_error("apply_channel_map: mapped channel name '" + out_name + "' normalizes to empty");
    }
    if (used_norm.find(out_key) != used_norm.end()) {
      throw std::runtime_error("apply_channel_map: duplicate mapped channel '" + out_name + "'");
    }
    used_norm.insert(out_key);

    new_names.push_back(out_name);
    new_data.push_back(rec->data[i]);
  }

  rec->channel_names = std::move(new_names);
  rec->data = std::move(new_data);
}

void write_channel_map_template(const std::string& path, const EEGRecording& rec) {
  std::ofstream o(path);
  if (!o) throw std::runtime_error("Failed to write channel map template: " + path);

  o << "old,new\n";
  for (const auto& name : rec.channel_names) {
    // Minimal CSV escaping: quote if it contains a comma or quote.
    bool needs_quote = (name.find(',') != std::string::npos) || (name.find('"') != std::string::npos);
    if (!needs_quote) {
      o << name << ",\n";
    } else {
      std::string esc = name;
      // Escape quotes by doubling them.
      size_t pos = 0;
      while ((pos = esc.find('"', pos)) != std::string::npos) {
        esc.insert(pos, 1, '"');
        pos += 2;
      }
      o << "\"" << esc << "\",\n";
    }
  }
}

} // namespace qeeg
