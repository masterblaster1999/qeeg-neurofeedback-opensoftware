#!/usr/bin/env python3
"""Shared helpers for the dependency-free HTML report renderers.

The render_*_report.py scripts in this repository intentionally aim to be:

  - dependency-free (Python stdlib only)
  - safe to open locally (self-contained HTML; no network requests)
  - robust to slightly-messy CSVs (extra whitespace, missing optional files)

To keep those scripts consistent and easier to maintain, a few small utilities
live here:
  - HTML escaping
  - safe JSON / text reads
  - a shared base CSS theme
  - a shared sortable-table JavaScript implementation
"""

from __future__ import annotations

import sys

# Avoid polluting the source tree with __pycache__ when these scripts are run
# directly or via CTest/CI. This is a developer convenience only; it does not
# change the outputs of the report renderers.
sys.dont_write_bytecode = True

import csv
import datetime as _dt
import html
import json
import math
import os
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


def _cleanup_stale_pycache() -> None:
    """Best-effort cleanup of stale scripts/__pycache__.

    Compiled Python bytecode is harmless, but it creates noisy diffs and can
    confuse users when it shows up in patch zips or bundles.
    """

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, '__pycache__')
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            # Typical filename: report_common.cpython-311.pyc
            if not fn.endswith('.pyc'):
                continue
            try:
                os.remove(os.path.join(pycache, fn))
                removed_any = True
            except Exception:
                pass

        if removed_any:
            try:
                if not os.listdir(pycache):
                    os.rmdir(pycache)
            except Exception:
                pass
    except Exception:
        pass


_cleanup_stale_pycache()


def e(x: Any) -> str:
    """HTML-escape any value for safe embedding in HTML."""
    return html.escape(str(x), quote=True)


def is_dir(path: str) -> bool:
    """Return True if path exists and is a directory (never raises)."""
    try:
        return os.path.isdir(path)
    except Exception:
        return False


def posix_relpath(path: str, start: str) -> str:
    """os.path.relpath with forward slashes (for HTML hrefs)."""
    rel = os.path.relpath(path, start)
    return rel.replace(os.sep, "/")


def utc_now_iso() -> str:
    """Return current UTC time as ISO-8601 with trailing 'Z'."""
    return _dt.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"


def read_json_if_exists(path: str) -> Optional[Dict[str, Any]]:
    """Load a JSON dict from path, returning None if missing/invalid."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            v = json.load(f)
        return v if isinstance(v, dict) else None
    except FileNotFoundError:
        return None
    except Exception:
        return None


def read_text_if_exists(path: str, *, max_bytes: int = 256_000) -> Optional[str]:
    """Read a UTF-8-ish text file, truncated to max_bytes if needed."""
    try:
        with open(path, "rb") as f:
            b = f.read(max_bytes + 1)
        truncated = len(b) > max_bytes
        if truncated:
            b = b[:max_bytes]
        try:
            txt = b.decode("utf-8")
        except Exception:
            txt = b.decode("utf-8", errors="replace")
        if truncated:
            txt += "\n... (truncated)\n"
        return txt
    except FileNotFoundError:
        return None
    except Exception:
        return None


def read_csv_dict(path: str) -> Tuple[List[str], List[Dict[str, str]]]:
    """Read a CSV/TSV into (headers, rows) using DictReader.

    Robustness notes:
      - Handles UTF-8 BOM via "utf-8-sig".
      - Accepts basic TSVs (".tsv") by switching the dialect.
      - Ignores stray/empty header columns.
      - Tolerates rows with too many columns by stashing extras under a
        DictReader "restkey" and discarding them.
    """

    # Basic delimiter support: most repo tools emit CSV, but a few related
    # outputs (e.g., BIDS-ish derived events) may be TSV.
    lower = path.lower()
    dialect: csv.Dialect = csv.excel_tab if lower.endswith(".tsv") else csv.excel

    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        r = csv.DictReader(f, dialect=dialect, restkey="__extra__", restval="")
        if not r.fieldnames:
            raise RuntimeError(f"Expected header row in CSV: {path}")

        headers = [str(h).strip() for h in r.fieldnames if h is not None]
        headers = [h for h in headers if h != ""]

        rows: List[Dict[str, str]] = []
        for row in r:
            out: Dict[str, str] = {}
            for k, v in row.items():
                if k is None:
                    # Unnamed extras (should be captured by restkey, but guard anyway).
                    continue
                ks = str(k).strip()
                if ks == "" or ks == "__extra__":
                    continue
                if isinstance(v, list):
                    # Extra columns captured by restkey may be a list.
                    vs = ", ".join(str(x).strip() for x in v if str(x).strip() != "")
                else:
                    vs = "" if v is None else str(v)
                out[ks] = vs.strip()
            rows.append(out)
        return headers, rows



def try_float(x: Any) -> float:
    """Best-effort float conversion.

    Returns:
        A finite float if possible; otherwise math.nan.
    """
    if x is None:
        return math.nan
    s = str(x).strip()
    if s == "":
        return math.nan
    try:
        return float(s)
    except Exception:
        return math.nan


_TRUE_STRS = {"1", "true", "t", "yes", "y", "on"}
_FALSE_STRS = {"0", "false", "f", "no", "n", "off"}


def try_bool_int(x: Any, *, default: int = 0) -> int:
    """Parse common boolean-ish values into 0/1.

    Accepts: 1/0, true/false, yes/no, on/off (case-insensitive).
    """
    if x is None:
        return int(default)
    s = str(x).strip().lower()
    if s == "":
        return int(default)
    if s in _TRUE_STRS:
        return 1
    if s in _FALSE_STRS:
        return 0
    # Try numeric fallback.
    try:
        v = float(s)
        return 1 if v != 0.0 else 0
    except Exception:
        return int(default)


def try_int(x: Any) -> Optional[int]:
    """Best-effort integer conversion.

    Returns:
        int if x is a number-like string; otherwise None.
    """
    if x is None:
        return None
    s = str(x).strip()
    if s == "":
        return None
    # Handle boolean-ish strings explicitly.
    sl = s.lower()
    if sl in _TRUE_STRS:
        return 1
    if sl in _FALSE_STRS:
        return 0
    try:
        return int(float(s))
    except Exception:
        return None


def downsample_indices(n: int, max_points: int) -> List[int]:
    """Return monotonically increasing indices selecting up to max_points from 0..n-1."""
    if n <= 0:
        return []
    max_points = max(1, int(max_points))
    if n <= max_points:
        return list(range(n))
    step = max(1, int(math.ceil(n / max_points)))
    idx = list(range(0, n, step))
    if idx and idx[-1] != n - 1:
        idx.append(n - 1)
    return idx


def finite_minmax(values: Sequence[float]) -> Tuple[float, float]:
    """Return (min, max) over finite values; defaults to (0, 1) if none are finite."""
    mn = math.inf
    mx = -math.inf
    for v in values:
        if math.isfinite(v):
            mn = min(mn, v)
            mx = max(mx, v)
    if not (math.isfinite(mn) and math.isfinite(mx)):
        return 0.0, 1.0
    if mn == mx:
        return mn - 1.0, mx + 1.0
    return mn, mx

BASE_CSS = r""":root {
  /* Default: dark theme */
  --bg:#0b0f14;
  --panel:rgba(17,24,39,0.78);
  --panel-strong:#0f1725;
  --panel-inset:#0b0f14;

  --text:#e7eefc;
  --muted:#9fb0c8;
  --grid:#233044;

  --accent:#8fb7ff;
  --link:var(--accent);

  --ok:#7fb3ff;
  --bad:#ff7fa3;
  --warn:#ffb86b;

  --note-bg: rgba(255,255,255,0.04);
  --note-border: rgba(255,255,255,0.12);
  --note-border-strong: rgba(255,255,255,0.10);
  --hover: rgba(255,255,255,0.03);
  --code-bg: rgba(255,255,255,0.06);
}

/* Explicit theme override (set by JS via data-theme). */
:root[data-theme="light"] {
  --bg:#f8fafc;
  --panel:rgba(255,255,255,0.92);
  --panel-strong:#e2e8f0;
  --panel-inset:#ffffff;

  --text:#0b1220;
  --muted:#475569;
  --grid:#cbd5e1;

  --accent:#1d4ed8;
  --link:var(--accent);

  --ok:#2563eb;
  --bad:#be123c;
  --warn:#b45309;

  --note-bg: rgba(0,0,0,0.03);
  --note-border: rgba(0,0,0,0.16);
  --note-border-strong: rgba(0,0,0,0.12);
  --hover: rgba(0,0,0,0.03);
  --code-bg: rgba(0,0,0,0.06);
}

/* Auto light theme if the OS prefers it and the user hasn't chosen an override. */
@media (prefers-color-scheme: light) {
  :root:not([data-theme]) {
    --bg:#f8fafc;
    --panel:rgba(255,255,255,0.92);
    --panel-strong:#e2e8f0;
    --panel-inset:#ffffff;

    --text:#0b1220;
    --muted:#475569;
    --grid:#cbd5e1;

    --accent:#1d4ed8;
    --link:var(--accent);

    --ok:#2563eb;
    --bad:#be123c;
    --warn:#b45309;

    --note-bg: rgba(0,0,0,0.03);
    --note-border: rgba(0,0,0,0.16);
    --note-border-strong: rgba(0,0,0,0.12);
    --hover: rgba(0,0,0,0.03);
    --code-bg: rgba(0,0,0,0.06);
  }
}

* { box-sizing: border-box; }

body {
  margin: 0;
  font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial, "Apple Color Emoji", "Segoe UI Emoji";
  background: var(--bg);
  color: var(--text);
}

header {
  padding: 18px 22px;
  border-bottom: 1px solid var(--grid);
  background: linear-gradient(180deg, rgba(143,183,255,0.12), rgba(17,24,39,0.0));
}

h1 { margin: 0; font-size: 20px; letter-spacing: 0.2px; }
.meta { margin-top: 6px; font-size: 12px; color: var(--muted); }

main { padding: 18px 22px 40px; max-width: 1200px; margin: 0 auto; }

.card {
  background: var(--panel);
  border: 1px solid var(--grid);
  border-radius: 12px;
  padding: 14px 16px;
  margin: 14px 0;
}

.card h2 { margin: 0 0 10px; font-size: 16px; }
.card h3 { margin: 0 0 10px; font-size: 14px; color: var(--text); }

.note {
  background: var(--note-bg);
  border: 1px dashed var(--note-border);
  border-radius: 10px;
  padding: 10px 12px;
  color: var(--muted);
  font-size: 13px;
}

.muted { color: var(--muted); }

.footer { margin-top: 18px; font-size: 12px; color: var(--muted); }

a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }

code {
  background: var(--code-bg);
  padding: 2px 6px;
  border-radius: 6px;
}

pre {
  background: var(--panel-inset);
  border: 1px solid var(--grid);
  border-radius: 10px;
  padding: 10px;
  margin: 0;
  overflow: auto;
  color: var(--text);
  white-space: pre-wrap;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.data-table th, .data-table td {
  border-bottom: 1px solid var(--note-border-strong);
  padding: 8px;
  vertical-align: top;
}

.data-table th {
  text-align: left;
  background: var(--panel-strong);
  color: var(--text);
  cursor: pointer;
  user-select: none;
}

/* Visual indicator for current sort direction (set by JS via data-asc) */
.data-table th[data-asc="1"]::after { content: " ▲"; color: var(--muted); font-size: 11px; }
.data-table th[data-asc="0"]::after { content: " ▼"; color: var(--muted); font-size: 11px; }

.data-table.sticky th {
  position: sticky;
  top: 0;
  z-index: 2;
}

.data-table tr:hover td { background: var(--hover); }

/* Small controls row used by several reports above large tables */
.table-controls {
  display: flex;
  gap: 10px;
  flex-wrap: wrap;
  align-items: center;
  margin: 10px 0 10px;
}

.table-controls input[type="search"] {
  width: min(520px, 100%);
  padding: 10px 12px;
  border-radius: 10px;
  border: 1px solid var(--grid);
  background: var(--panel-inset);
  color: var(--text);
}

.table-controls button {
  padding: 10px 12px;
  border-radius: 10px;
  border: 1px solid var(--grid);
  background: var(--panel-strong);
  color: var(--text);
  cursor: pointer;
  font-size: 13px;
}

.table-controls button:hover { background: rgba(143,183,255,0.10); }
.table-controls button:active { transform: translateY(1px); }

.table-controls .filter-count { font-size: 12px; color: var(--muted); }

.table-wrap {
  overflow: auto;
  border-radius: 10px;
  border: 1px solid var(--grid);
}

.table-wrap .data-table { margin: 0; }

/* Column chooser / table UX helpers (added via JS at runtime). */
.qeeg-col-hidden { display: none !important; }

details.col-chooser, details.filter-help {
  border: 1px solid var(--grid);
  border-radius: 10px;
  background: var(--note-bg);
}

details.col-chooser > summary,
details.filter-help > summary {
  padding: 8px 10px;
  cursor: pointer;
  user-select: none;
  list-style: none;
  color: var(--text);
  font-size: 13px;
}

details.col-chooser > summary::-webkit-details-marker,
details.filter-help > summary::-webkit-details-marker { display: none; }

details.col-chooser[open] > summary,
details.filter-help[open] > summary {
  border-bottom: 1px solid var(--note-border-strong);
}

.col-chooser-box,
.filter-help-box {
  padding: 10px 10px 12px;
}

.col-chooser-actions {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
  margin: 0 0 10px;
}

.col-chooser-box input[type="search"] {
  width: min(420px, 100%);
  padding: 8px 10px;
  border-radius: 10px;
  border: 1px solid var(--grid);
  background: var(--panel-inset);
  color: var(--text);
  margin: 0 0 10px;
}

.col-chooser-list {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 6px 10px;
  max-height: 260px;
  overflow: auto;
  padding-right: 4px;
}

.col-chooser-item {
  display: flex;
  gap: 8px;
  align-items: center;
  font-size: 12px;
  color: var(--muted);
}

.col-chooser-item input { transform: translateY(0.5px); }

/* Floating theme toggle (auto/dark/light) - injected by JS. */
.qeeg-theme-toggle {
  position: fixed;
  top: 14px;
  right: 14px;
  z-index: 9999;
  padding: 8px 10px;
  border-radius: 10px;
  border: 1px solid var(--grid);
  background: var(--panel-strong);
  color: var(--text);
  cursor: pointer;
  font-size: 12px;
  opacity: 0.92;
}

.qeeg-theme-toggle:hover { background: rgba(143,183,255,0.10); opacity: 1.0; }
.qeeg-theme-toggle:active { transform: translateY(1px); }

@media print {
  .qeeg-theme-toggle { display: none; }
}
"""


JS_SORT_TABLE_BASE = r"""
// Minimal, dependency-free sortable/filterable HTML table helpers.
//
// Usage:
//   - Add onclick=\"sortTable(this)\" to <th> elements.
//   - Wrap a <table> in a .table-filter container with an <input type=search>
//     that calls filterTable(this) oninput.
//
// Filter syntax (compatible with plain substring search):
//   - plain text:            fz
//   - exclude token:         -fz
//   - column contains:       channel:fz
//   - column equals:         bad=1
//   - numeric comparisons:   value>0.3  duration_sec>=2
//
// Notes:
//   - Column names are matched against header text, case-insensitive, with
//     whitespace/punctuation ignored (\"abs corr\" == \"abs_corr\" == \"abscorr\").
//   - Tokens are AND'ed together.
//   - If a token looks like a column filter but the column name doesn't match
//     any header, it falls back to a plain substring search.

function _parseMaybeNumber(s) {
  if (s === null || s === undefined) return NaN;
  if (typeof s !== 'string') s = String(s);
  s = s.trim();
  if (!s) return NaN;
  // remove common thousands separators
  s = s.replace(/,/g, '');
  // allow trailing percent sign
  s = s.replace(/%\s*$/, '');
  const v = Number(s);
  return isNaN(v) ? NaN : v;
}

function sortTable(th) {
  const table = th ? th.closest('table') : null;
  if (!table) return;
  const tbody = table.querySelector('tbody');
  if (!tbody) return;

  const idx = Array.prototype.indexOf.call(th.parentNode.children, th);
  const rows = Array.from(tbody.querySelectorAll('tr'));

  // Clear any previous sort indicators in this table.
  try {
    table.querySelectorAll('thead th').forEach(other => {
      if (other !== th) {
        try { delete other.dataset.asc; delete other.dataset.sortAsc; } catch(e) {}
        try { other.removeAttribute('aria-sort'); } catch(e) {}
      }
    });
  } catch (e) {}

  // Toggle direction. We use data-asc=\"1\" / \"0\" so CSS can show ▲/▼.
  // (data-sort-asc is kept only for backward compatibility with older reports.)
  const asc = !(th.dataset.asc === '1' || th.dataset.sortAsc === 'true');
  th.dataset.asc = asc ? '1' : '0';
  th.dataset.sortAsc = asc ? 'true' : 'false';
  try { th.setAttribute('aria-sort', asc ? 'ascending' : 'descending'); } catch(e) {}

  rows.sort((a, b) => {
    const ac = a.children[idx];
    const bc = b.children[idx];
    const at = ac ? (ac.getAttribute('data-num') ?? ac.innerText) : '';
    const bt = bc ? (bc.getAttribute('data-num') ?? bc.innerText) : '';

    // try numeric
    const an = _parseMaybeNumber(at);
    const bn = _parseMaybeNumber(bt);
    if (!isNaN(an) && !isNaN(bn)) {
      return asc ? (an - bn) : (bn - an);
    }
    // string compare
    return asc ? String(at).localeCompare(String(bt)) : String(bt).localeCompare(String(at));
  });

  // remove old rows and re-append in new order
  rows.forEach(r => tbody.appendChild(r));
}

function _normalizeHeaderName(s) {
  if (s === null || s === undefined) return '';
  return String(s).toLowerCase().replace(/[^a-z0-9]+/g, '');
}

function _getHeaderMap(table) {
  // Cache on the table element.
  if (table && table._qeegHeaderMap) return table._qeegHeaderMap;
  const map = {};
  if (!table) return map;
  const ths = table.querySelectorAll('thead th');
  ths.forEach((th, idx) => {
    const key = _normalizeHeaderName(th.textContent || '');
    if (key && map[key] === undefined) map[key] = idx;
  });
  table._qeegHeaderMap = map;
  return map;
}

function _tokenizeFilterQuery(q) {
  const tokens = [];
  if (!q) return tokens;
  let cur = '';
  let quote = null;
  for (let i = 0; i < q.length; i++) {
    const ch = q[i];
    if (quote) {
      if (ch === quote) {
        quote = null;
      } else {
        cur += ch;
      }
      continue;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
      continue;
    }
    if (/\s/.test(ch)) {
      if (cur.trim() !== '') {
        tokens.push(cur.trim());
        cur = '';
      }
      continue;
    }
    cur += ch;
  }
  if (cur.trim() !== '') tokens.push(cur.trim());
  return tokens;
}

function _cellText(td) {
  return ((td && td.textContent) ? td.textContent : '').toLowerCase().trim();
}

function _cellNumber(td) {
  if (!td) return NaN;
  const numAttr = td.getAttribute('data-num');
  if (numAttr !== null && numAttr !== undefined && String(numAttr).trim() !== '') {
    const v = _parseMaybeNumber(numAttr);
    if (!isNaN(v)) return v;
  }
  return _parseMaybeNumber(td.textContent || '');
}

function _parseTokenToCond(tok, headerMap, warnings) {
  // Returns a condition object:
  //   { kind: 'row'|'col', neg: bool, op: string, value: string, idx?: number }
  let t = (tok || '').trim();
  if (!t) return null;

  let neg = false;
  if (t[0] === '-') {
    neg = true;
    t = t.slice(1).trim();
  } else if (t[0] === '!' && t.length > 1 && t[1] !== '=') {
    // Allow !token as a negation shorthand (but avoid conflict with !=)
    neg = true;
    t = t.slice(1).trim();
  }
  if (!t) return null;

  const ops = ['>=', '<=', '!=', '>', '<', '='];
  for (let k = 0; k < ops.length; k++) {
    const op = ops[k];
    const j = t.indexOf(op);
    if (j > 0 && j < t.length - op.length) {
      const left = t.slice(0, j).trim();
      const right = t.slice(j + op.length).trim();
      if (!left || !right) break;
      const key = _normalizeHeaderName(left);
      if (key && headerMap && headerMap[key] !== undefined) {
        return { kind: 'col', neg: neg, op: op, idx: headerMap[key], value: right };
      } else {
        // Looks like a column filter but we can't resolve the column; fall back to substring search
        if (warnings && key) warnings.add(left);
        return { kind: 'row', neg: neg, op: 'contains', value: t.toLowerCase() };
      }
    }
  }

  // Column-contains with ':' (only if column exists). Do NOT warn on unknown columns
  // because ':' is also common in values (e.g., channel pairs like F3:F4).
  const c = t.indexOf(':');
  if (c > 0 && c < t.length - 1) {
    const left = t.slice(0, c).trim();
    const right = t.slice(c + 1).trim();
    const key = _normalizeHeaderName(left);
    if (key && headerMap && headerMap[key] !== undefined) {
      return { kind: 'col', neg: neg, op: ':', idx: headerMap[key], value: right };
    }
    // else: fall back to substring search
  }

  return { kind: 'row', neg: neg, op: 'contains', value: t.toLowerCase() };
}

function _rowMatchesConds(tr, conds) {
  if (!conds || conds.length === 0) return true;
  const rowText = tr ? (tr.innerText || '').toLowerCase() : '';
  for (let i = 0; i < conds.length; i++) {
    const c = conds[i];
    if (!c) continue;
    let ok = true;
    if (c.kind === 'row') {
      ok = (rowText.indexOf(c.value) !== -1);
    } else if (c.kind === 'col') {
      const td = tr.children ? tr.children[c.idx] : null;
      const vtxt = _cellText(td);
      const want = (c.value || '').toLowerCase();

      if (c.op === ':') {
        ok = (vtxt.indexOf(want) !== -1);
      } else if (c.op === '=' || c.op === '!=') {
        // Try numeric equality if both parse; else string equality
        const a = _cellNumber(td);
        const b = _parseMaybeNumber(c.value);
        if (!isNaN(a) && !isNaN(b)) {
          ok = (a === b);
        } else {
          ok = (vtxt === want);
        }
        if (c.op === '!=') ok = !ok;
      } else if (c.op === '>' || c.op === '<' || c.op === '>=' || c.op === '<=') {
        const a = _cellNumber(td);
        const b = _parseMaybeNumber(c.value);
        if (isNaN(a) || isNaN(b)) {
          ok = false;
        } else {
          if (c.op === '>') ok = (a > b);
          else if (c.op === '<') ok = (a < b);
          else if (c.op === '>=') ok = (a >= b);
          else if (c.op === '<=') ok = (a <= b);
        }
      } else {
        // fallback: contains
        ok = (vtxt.indexOf(want) !== -1);
      }
    }
    if (c.neg) ok = !ok;
    if (!ok) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Lightweight state helpers (best-effort, safe on file://).
// ---------------------------------------------------------------------------

function _qeegStorage() {
  try { if (window && window.localStorage) return window.localStorage; } catch(e) {}
  try { if (window && window.sessionStorage) return window.sessionStorage; } catch(e) {}
  return null;
}

function _qeegStorageGet(key) {
  const s = _qeegStorage();
  if (!s) return null;
  try { return s.getItem(key); } catch(e) { return null; }
}

function _qeegStorageSet(key, value) {
  const s = _qeegStorage();
  if (!s) return;
  try { s.setItem(key, value); } catch(e) {}
}

function _qeegStorageRemove(key) {
  const s = _qeegStorage();
  if (!s) return;
  try { s.removeItem(key); } catch(e) {}
}

function _qeegGetTableKey(table) {
  if (!table) return '';
  if (table._qeegKey) return table._qeegKey;

  // Ensure a stable id per table (use existing id if present).
  let id = '';
  try { id = table.getAttribute('id') || ''; } catch(e) { id = ''; }
  if (!id) {
    // Derive a deterministic id based on the table's index in the document.
    try {
      const all = document.querySelectorAll('table');
      const idx = Array.prototype.indexOf.call(all, table);
      id = 'qeeg_table_' + (idx >= 0 ? idx : 0);
    } catch (e) {
      id = 'qeeg_table_0';
    }
    try { table.setAttribute('id', id); } catch(e) {}
  }

  let page = '';
  try { page = (location && location.href) ? String(location.href) : ''; } catch(e) { page = ''; }
  if (!page) {
    try { page = (document && document.title) ? String(document.title) : 'qeeg'; } catch(e) { page = 'qeeg'; }
  }

  const key = 'qeeg::' + page + '::' + id;
  table._qeegKey = key;
  return key;
}

function _qeegKeyFilter(table) { return _qeegGetTableKey(table) + '::filter'; }
function _qeegKeyHiddenCols(table) { return _qeegGetTableKey(table) + '::hiddenCols'; }

// ---------------------------------------------------------------------------
// Table filter behavior (search box).
// ---------------------------------------------------------------------------

function filterTable(input) {
  const wrap = input ? input.closest('.table-filter') : null;
  const table = wrap ? wrap.querySelector('table') : null;
  const counter = wrap ? wrap.querySelector('.filter-count') : null;
  if (!table) return;

  const q = (input && input.value ? input.value : '').trim();
  const headerMap = _getHeaderMap(table);

  const warnings = new Set();
  const tokens = _tokenizeFilterQuery(q);
  const conds = [];
  for (let i = 0; i < tokens.length; i++) {
    const cond = _parseTokenToCond(tokens[i], headerMap, warnings);
    if (cond) conds.push(cond);
  }

  let shown = 0;
  let total = 0;
  const rows = table.querySelectorAll('tbody tr');
  rows.forEach(tr => {
    total++;
    let ok = true;
    if (q !== '') {
      ok = _rowMatchesConds(tr, conds);
    }
    tr.style.display = ok ? '' : 'none';
    if (ok) shown++;
  });

  if (counter) {
    if (q === '') {
      counter.textContent = '';
    } else {
      let msg = `shown ${shown} / ${total}`;
      if (warnings && warnings.size) {
        // Keep it short.
        const arr = Array.from(warnings).slice(0, 4);
        msg += ` — unknown columns: ${arr.join(', ')}`;
        if (warnings.size > arr.length) msg += '…';
      }
      counter.textContent = msg;
    }
  }

  // Persist the current query (best-effort).
  try { _qeegStorageSet(_qeegKeyFilter(table), q); } catch(e) {}
}

function _csvEscapeCell(s) {
  if (s === null || s === undefined) return '';
  const t = String(s);
  if (t.indexOf('"') !== -1 || t.indexOf(',') !== -1 || t.indexOf('\n') !== -1) {
    return '"' + t.replace(/"/g, '""') + '"';
  }
  return t;
}

// Column visibility helpers (used by column chooser and CSV export).
function _qeegIsHiddenCell(cell) {
  if (!cell) return false;
  try {
    if (cell.classList && cell.classList.contains('qeeg-col-hidden')) return true;
    if (cell.style && cell.style.display === 'none') return true;
  } catch (e) {}
  return false;
}

function _qeegSetColumnHidden(table, idx, hidden) {
  if (!table) return;
  if (idx === null || idx === undefined) return;
  try {
    const rows = table.querySelectorAll('tr');
    rows.forEach(tr => {
      const cell = (tr.children && tr.children.length > idx) ? tr.children[idx] : null;
      if (!cell) return;
      if (hidden) cell.classList.add('qeeg-col-hidden');
      else cell.classList.remove('qeeg-col-hidden');
    });
  } catch (e) {}
}

function _qeegHiddenColumnKeys(table) {
  const out = [];
  if (!table) return out;
  try {
    const ths = table.querySelectorAll('thead th');
    ths.forEach(th => {
      const k = _normalizeHeaderName(th.textContent || '');
      if (!k) return;
      if (_qeegIsHiddenCell(th)) out.push(k);
    });
  } catch(e) {}
  return out;
}

function _qeegSaveHiddenColumns(table) {
  try {
    const arr = _qeegHiddenColumnKeys(table);
    _qeegStorageSet(_qeegKeyHiddenCols(table), JSON.stringify(arr));
  } catch(e) {}
}

function _qeegLoadHiddenColumns(table) {
  try {
    const raw = _qeegStorageGet(_qeegKeyHiddenCols(table));
    if (!raw) return null;
    const arr = JSON.parse(raw);
    if (!Array.isArray(arr)) return null;
    const set = new Set();
    arr.forEach(x => { if (x !== null && x !== undefined) set.add(String(x)); });
    return set;
  } catch(e) { return null; }
}

function _qeegClearHiddenColumns(table) {
  try { _qeegStorageRemove(_qeegKeyHiddenCols(table)); } catch(e) {}
}

function tableToCSV(table, onlyVisibleRows) {
  const lines = [];
  const rows = table.querySelectorAll('tr');
  rows.forEach(tr => {
    if (onlyVisibleRows && tr.style && tr.style.display === 'none') return;
    const cells = tr.querySelectorAll('th,td');
    const vals = [];
    cells.forEach(td => {
      if (_qeegIsHiddenCell(td)) return;
      let v = '';
      const dataCsv = td.getAttribute('data-csv');
      if (dataCsv !== null && dataCsv !== undefined) {
        v = dataCsv;
      } else {
        const dataNum = td.getAttribute('data-num');
        if (dataNum !== null && dataNum !== undefined && String(dataNum).trim() !== '') {
          v = dataNum;
        } else {
          v = (td.textContent || '').trim();
        }
      }
      vals.push(_csvEscapeCell(v));
    });
    if (vals.length > 0) lines.push(vals.join(','));
  });
  return lines.join('\n') + '\n';
}

function downloadTextFile(name, text, type) {
  try {
    const blob = new Blob([text], {type: type});
    if (window.navigator && window.navigator.msSaveOrOpenBlob) {
      window.navigator.msSaveOrOpenBlob(blob, name);
      return;
    }
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
      try { document.body.removeChild(a); } catch(e) {}
      try { URL.revokeObjectURL(url); } catch(e) {}
    }, 0);
  } catch (e) {
    // Fallback (may fail on large data)
    const uri = 'data:' + type + ',' + encodeURIComponent(text);
    window.open(uri, '_blank');
  }
}

function downloadTableCSV(el, filename, onlyVisible) {
  const wrap = el ? el.closest('.table-filter') : null;
  const table = wrap ? wrap.querySelector('table') : null;
  if (!table) return;
  const csv = tableToCSV(table, (onlyVisible !== false));
  downloadTextFile(filename || 'table.csv', csv, 'text/csv;charset=utf-8;');
}

// ---------------------------------------------------------------------------
// Extra UX helpers (column chooser + built-in filter help + clear button).
// Inserted at runtime for each .table-filter wrapper.
// ---------------------------------------------------------------------------

function _qeegInitClearButtonForWrap(wrap) {
  if (!wrap) return;
  const controls = wrap.querySelector('.table-controls');
  if (!controls) return;
  const inp = controls.querySelector('input[type="search"]');
  if (!inp) return;

  if (controls.querySelector('button.qeeg-clear-filter')) return;

  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'qeeg-clear-filter';
  btn.textContent = 'Clear';
  btn.addEventListener('click', () => {
    inp.value = '';
    filterTable(inp);
    try { inp.focus(); } catch(e) {}
  });

  try { inp.insertAdjacentElement('afterend', btn); } catch(e) {
    // fallback append
    controls.appendChild(btn);
  }
}

function _qeegInitColumnChooserForWrap(wrap) {
  if (!wrap) return;
  const table = wrap.querySelector('table');
  const controls = wrap.querySelector('.table-controls');
  if (!table || !controls) return;

  // Avoid duplicating.
  if (controls.querySelector('details.col-chooser')) return;

  const ths = table.querySelectorAll('thead th');
  if (!ths || ths.length <= 1) return;

  // Apply any persisted hidden column state before building the chooser UI.
  const savedHidden = _qeegLoadHiddenColumns(table);
  if (savedHidden) {
    try {
      ths.forEach((th, idx) => {
        const k = _normalizeHeaderName(th.textContent || '');
        if (k && savedHidden.has(k)) _qeegSetColumnHidden(table, idx, true);
      });
    } catch(e) {}
  }

  const details = document.createElement('details');
  details.className = 'col-chooser';

  const summary = document.createElement('summary');
  summary.textContent = 'Columns';
  details.appendChild(summary);

  const box = document.createElement('div');
  box.className = 'col-chooser-box';

  const actions = document.createElement('div');
  actions.className = 'col-chooser-actions';

  const btnAll = document.createElement('button');
  btnAll.type = 'button';
  btnAll.textContent = 'All';

  const btnNone = document.createElement('button');
  btnNone.type = 'button';
  btnNone.textContent = 'None';

  const btnInvert = document.createElement('button');
  btnInvert.type = 'button';
  btnInvert.textContent = 'Invert';

  const btnReset = document.createElement('button');
  btnReset.type = 'button';
  btnReset.textContent = 'Reset';

  actions.appendChild(btnAll);
  actions.appendChild(btnNone);
  actions.appendChild(btnInvert);
  actions.appendChild(btnReset);

  const search = document.createElement('input');
  search.type = 'search';
  search.placeholder = 'Find column…';

  const list = document.createElement('div');
  list.className = 'col-chooser-list';

  const items = [];

  ths.forEach((th, idx) => {
    const labelText = ((th.textContent || '').trim()) || ('col ' + (idx + 1));
    const label = document.createElement('label');
    label.className = 'col-chooser-item';

    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = !_qeegIsHiddenCell(th);

    const span = document.createElement('span');
    span.textContent = labelText;

    cb.addEventListener('change', () => {
      _qeegSetColumnHidden(table, idx, !cb.checked);
      _qeegSaveHiddenColumns(table);
    });

    label.appendChild(cb);
    label.appendChild(span);
    list.appendChild(label);

    items.push({ label: labelText.toLowerCase(), el: label, cb: cb, idx: idx });
  });

  function applySearch() {
    const q = (search.value || '').trim().toLowerCase();
    items.forEach(it => {
      const ok = (q === '') || (it.label.indexOf(q) !== -1);
      it.el.style.display = ok ? '' : 'none';
    });
  }

  search.addEventListener('input', applySearch);
  search.addEventListener('keydown', (ev) => {
    if (ev.key === 'Escape') {
      search.value = '';
      applySearch();
    }
  });

  function setAllChecked(checked) {
    items.forEach(it => {
      it.cb.checked = checked;
      _qeegSetColumnHidden(table, it.idx, !checked);
    });
    _qeegSaveHiddenColumns(table);
  }

  btnAll.addEventListener('click', () => { setAllChecked(true); applySearch(); });
  btnNone.addEventListener('click', () => { setAllChecked(false); applySearch(); });
  btnInvert.addEventListener('click', () => {
    items.forEach(it => {
      const next = !it.cb.checked;
      it.cb.checked = next;
      _qeegSetColumnHidden(table, it.idx, !next);
    });
    _qeegSaveHiddenColumns(table);
    applySearch();
  });

  btnReset.addEventListener('click', () => {
    _qeegClearHiddenColumns(table);
    setAllChecked(true);
    applySearch();
  });

  box.appendChild(actions);
  box.appendChild(search);
  box.appendChild(list);
  details.appendChild(box);

  controls.appendChild(details);
}

function _qeegInitFilterHelpForWrap(wrap) {
  if (!wrap) return;
  const controls = wrap.querySelector('.table-controls');
  if (!controls) return;

  if (controls.querySelector('details.filter-help')) return;

  const details = document.createElement('details');
  details.className = 'filter-help';

  const summary = document.createElement('summary');
  summary.textContent = 'Filter help';
  details.appendChild(summary);

  const box = document.createElement('div');
  box.className = 'filter-help-box';

  // Keep it short and readable. (HTML is constant, not user input.)
  box.innerHTML =
    '<div class="note">' +
      '<div><b>Filter syntax</b> (tokens are AND&#39;ed):</div>' +
      '<ul style="margin:8px 0 0 18px; padding:0;">' +
        '<li>Plain text: <code>fz</code></li>' +
        '<li>Exclude: <code>-bad</code></li>' +
        '<li>Column contains: <code>channel:Fz</code></li>' +
        '<li>Column equals: <code>bad=1</code></li>' +
        '<li>Numeric: <code>value&gt;0.3</code>, <code>duration_sec&gt;=2</code></li>' +
        '<li>Quotes for spaces: <code>"line noise"</code></li>' +
      '</ul>' +
    '</div>';

  details.appendChild(box);
  controls.appendChild(details);
}

function _qeegRestoreFilterForWrap(wrap) {
  if (!wrap) return;
  const table = wrap.querySelector('table');
  const inp = wrap.querySelector('input[type="search"]');
  if (!table || !inp) return;
  try {
    const raw = _qeegStorageGet(_qeegKeyFilter(table));
    if (raw !== null && raw !== undefined && String(raw).trim() !== '') {
      inp.value = String(raw);
      filterTable(inp);
    }
  } catch(e) {}
}

function qeeg_init_table_enhancements() {
  try {
    document.querySelectorAll('.table-filter').forEach(wrap => {
      _qeegInitClearButtonForWrap(wrap);
      _qeegInitColumnChooserForWrap(wrap);
      _qeegInitFilterHelpForWrap(wrap);
      _qeegRestoreFilterForWrap(wrap);
    });
  } catch (e) {}
}

// Convenience: allow clearing any report table filter with Escape.
function _qeegBindEscapeToFilterInputs() {
  try {
    document.querySelectorAll('.table-filter input[type="search"]').forEach(inp => {
      if (inp._qeegEscapeBound) return;
      inp._qeegEscapeBound = true;
      inp.addEventListener('keydown', (ev) => {
        if (ev.key === 'Escape') {
          inp.value = '';
          filterTable(inp);
        }
      });
    });
  } catch (e) {}
}

// Run init regardless of whether DOMContentLoaded already fired.
function qeeg_onReady(fn) {
  try {
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', fn);
    } else {
      fn();
    }
  } catch (e) {
    try { fn(); } catch(e2) {}
  }
}

qeeg_onReady(() => {
  _qeegBindEscapeToFilterInputs();
  qeeg_init_table_enhancements();
});
"""

# Standalone theme toggle (for reports that don't use JS_SORT_TABLE).
JS_THEME_TOGGLE = r"""
// ---------------------------------------------------------------------------
// Theme toggle (auto / dark / light).
//
// - Default: follow the OS color scheme (CSS prefers-color-scheme).
// - Click the floating button to cycle: auto -> dark -> light -> auto.
// - Preference is persisted in localStorage when available.
//
// Note: This snippet intentionally avoids registering its own DOMContentLoaded
// handler when embedded into JS_SORT_TABLE. The table helper already uses a
// single onReady hook (qeeg_onReady).
// ---------------------------------------------------------------------------
(function() {
  const KEY = 'qeeg::theme';

  function safeStorage() {
    try { return window.localStorage; } catch (e) { return null; }
  }

  function getThemeAttr() {
    try {
      const t = document.documentElement.getAttribute('data-theme');
      return t ? String(t) : '';
    } catch (e) {
      return '';
    }
  }

  function setThemeAttr(t) {
    try {
      if (!t) document.documentElement.removeAttribute('data-theme');
      else document.documentElement.setAttribute('data-theme', t);
    } catch (e) {}
  }

  function labelFor(t) {
    if (t === 'dark') return 'Theme: dark';
    if (t === 'light') return 'Theme: light';
    return 'Theme: auto';
  }

  function nextTheme(t) {
    if (t === '') return 'dark';
    if (t === 'dark') return 'light';
    return '';
  }

  function applySavedTheme() {
    const s = safeStorage();
    if (!s) return;
    try {
      const v = s.getItem(KEY);
      if (v === 'dark' || v === 'light') setThemeAttr(v);
    } catch (e) {}
  }

  function persistTheme(t) {
    const s = safeStorage();
    if (!s) return;
    try {
      if (!t) s.removeItem(KEY);
      else s.setItem(KEY, t);
    } catch (e) {}
  }

  function ensureButton() {
    try {
      if (!document || !document.body) return;
      if (document.querySelector('.qeeg-theme-toggle')) return;

      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'qeeg-theme-toggle';
      btn.title = 'Toggle color theme (auto / dark / light)';
      btn.textContent = labelFor(getThemeAttr());

      btn.addEventListener('click', () => {
        const cur = getThemeAttr();
        const nxt = nextTheme(cur);
        setThemeAttr(nxt);
        persistTheme(nxt);
        btn.textContent = labelFor(getThemeAttr());
      });

      document.body.appendChild(btn);
    } catch (e) {}
  }

  function init() {
    applySavedTheme();
    ensureButton();
  }

  // Prefer the shared onReady helper when available; otherwise run immediately
  // (most reports embed this script at the end of <body>).
  try {
    if (typeof qeeg_onReady === 'function') {
      qeeg_onReady(init);
    } else {
      init();
      // If body wasn't available yet, try again on the next tick.
      if (!document || !document.body) {
        try { setTimeout(init, 0); } catch (e) {}
      }
    }
  } catch (e) {
    try { init(); } catch (e2) {}
  }
})();
"""

# Back-compat export: most reports import JS_SORT_TABLE, which now includes the theme toggle.
JS_SORT_TABLE = JS_SORT_TABLE_BASE + "\n\n" + JS_THEME_TOGGLE
