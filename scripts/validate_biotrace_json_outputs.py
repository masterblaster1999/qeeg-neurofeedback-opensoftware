#!/usr/bin/env python3
"""Validate BioTrace helper JSON outputs against the repo's JSON Schemas.

This repository includes two small Python helpers for Mind Media BioTrace+ / NeXus
exports that happen to be ZIP-like containers:

  - scripts/biotrace_extract_container.py
      * --list-json      (candidate listing)
      * --print-json     (extraction manifest)

  - scripts/biotrace_run_nf.py
      * --list-json      (candidate listing)

This validator is intended for:
  - CI: catching accidental breaking changes to these machine-readable outputs.
  - Developers: quickly sanity-checking JSON outputs while iterating.

It prefers full JSON Schema validation using python-jsonschema (Draft 2020-12)
when available. If jsonschema is not installed, it falls back to a small,
hand-written structural check for the keys/types used by these outputs.

No third-party dependencies are required for the fallback mode.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional


# Best-effort: avoid polluting the source tree with __pycache__ when this script
# is run from CI/CTest.
sys.dont_write_bytecode = True


def _cleanup_stale_pycache() -> None:
    """Remove stale bytecode caches for this script (best effort)."""

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        pycache = os.path.join(here, "__pycache__")
        if not os.path.isdir(pycache):
            return

        removed_any = False
        for fn in os.listdir(pycache):
            if fn.startswith("validate_biotrace_json_outputs.") and fn.endswith(".pyc"):
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


def _run_capture(cmd: List[str], *, cwd: Path, env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )


def _json_from_stdout(stdout: str) -> Any:
    s = (stdout or "").strip()
    try:
        return json.loads(s)
    except json.JSONDecodeError as e:
        raise AssertionError(f"Expected pure JSON on stdout, but parse failed: {e}\n--- stdout ---\n{stdout}")


def _load_schema(path: Path) -> Dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise AssertionError(f"Schema is not an object: {path}")
    return data


def _validate_with_jsonschema(schema: Dict[str, Any], instance: Any) -> Optional[str]:
    """Return None on success, otherwise an error string."""

    try:
        import jsonschema  # type: ignore

        # Draft 2020-12 schemas are used in this repo.
        validator = jsonschema.Draft202012Validator(schema)
        validator.validate(instance)
        return None
    except ImportError:
        return "jsonschema not installed"
    except Exception as e:
        return str(e)


def _is_str_list(x: Any) -> bool:
    return isinstance(x, list) and all(isinstance(v, str) for v in x)


def _fallback_validate_candidates(obj: Any) -> None:
    if not isinstance(obj, dict):
        raise AssertionError("candidate listing output is not an object")
    for k in ("input", "prefer_exts", "candidates"):
        if k not in obj:
            raise AssertionError(f"missing required key: {k}")

    if not isinstance(obj.get("input"), str) or not obj["input"]:
        raise AssertionError("input must be a non-empty string")
    if not _is_str_list(obj.get("prefer_exts")):
        raise AssertionError("prefer_exts must be a list of strings")

    cands = obj.get("candidates")
    if not isinstance(cands, list):
        raise AssertionError("candidates must be a list")

    for c in cands:
        if not isinstance(c, dict):
            raise AssertionError("candidate entry must be an object")
        for k in ("index", "name", "ext", "size", "mtime"):
            if k not in c:
                raise AssertionError(f"candidate missing key: {k}")
        if not isinstance(c["index"], int) or c["index"] < 1:
            raise AssertionError("candidate.index must be int >= 1")
        if not isinstance(c["name"], str) or not c["name"]:
            raise AssertionError("candidate.name must be a non-empty string")
        if not isinstance(c["ext"], str):
            raise AssertionError("candidate.ext must be a string")
        if not isinstance(c["size"], int) or c["size"] < 0:
            raise AssertionError("candidate.size must be int >= 0")
        if c["mtime"] is not None and not isinstance(c["mtime"], str):
            raise AssertionError("candidate.mtime must be string or null")

    if "error" in obj and not isinstance(obj["error"], str):
        raise AssertionError("error must be a string when present")


def _fallback_validate_manifest(obj: Any) -> None:
    if not isinstance(obj, dict):
        raise AssertionError("manifest output is not an object")
    for k in ("input", "outdir", "main", "extracted"):
        if k not in obj:
            raise AssertionError(f"missing required key: {k}")

    if not isinstance(obj.get("input"), str) or not obj["input"]:
        raise AssertionError("input must be a non-empty string")
    if not isinstance(obj.get("outdir"), str) or not obj["outdir"]:
        raise AssertionError("outdir must be a non-empty string")

    if obj["main"] is not None and not isinstance(obj["main"], str):
        raise AssertionError("main must be a string or null")

    extracted = obj.get("extracted")
    if not _is_str_list(extracted):
        raise AssertionError("extracted must be a list of strings")

    if "error" in obj and not isinstance(obj["error"], str):
        raise AssertionError("error must be a string when present")


def _make_brainvision_triplet(base_name: str) -> tuple[str, bytes, str]:
    vhdr = (
        "Brain Vision Data Exchange Header File Version 1.0\n"
        "[Common Infos]\n"
        f"DataFile={base_name}.eeg\n"
        f"MarkerFile={base_name}.vmrk\n"
        "DataFormat=BINARY\n"
    )
    eeg = b"EEGDUMMY" + b"\x01\x02\x03\x04" * 16
    vmrk = (
        "Brain Vision Data Exchange Marker File Version 1.0\n"
        "[Common Infos]\n"
        f"DataFile={base_name}.eeg\n"
        "[Marker Infos]\n"
        "Mk1=Stimulus,S 1,1,1,0\n"
    )
    return vhdr, eeg, vmrk


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Validate BioTrace helper JSON outputs against schemas/")
    ap.add_argument(
        "--schemas-dir",
        default="schemas",
        help="Path to the repo's schemas/ directory (default: schemas)",
    )
    ap.add_argument("--verbose", action="store_true", help="Print extra debug output")
    ns = ap.parse_args(argv)

    repo_root = Path(__file__).resolve().parents[1]
    schemas_dir = (repo_root / ns.schemas_dir).resolve() if not Path(ns.schemas_dir).is_absolute() else Path(ns.schemas_dir).resolve()

    if not schemas_dir.exists():
        print(f"Error: schemas dir not found: {schemas_dir}", file=sys.stderr)
        return 2

    schema_extract_list = _load_schema(schemas_dir / "biotrace_extract_list.schema.json")
    schema_extract_manifest = _load_schema(schemas_dir / "biotrace_extract_manifest.schema.json")
    schema_run_nf_list = _load_schema(schemas_dir / "biotrace_run_nf_list.schema.json")

    # Prefer jsonschema if available.
    have_jsonschema = True
    try:
        import jsonschema  # noqa: F401
    except Exception:
        have_jsonschema = False

    def validate_candidates(which: str, obj: Any, schema: Dict[str, Any]) -> None:
        if have_jsonschema:
            err = _validate_with_jsonschema(schema, obj)
            if err is not None:
                raise AssertionError(f"{which}: jsonschema validation failed: {err}\nInstance: {json.dumps(obj, indent=2, sort_keys=True)}")
        else:
            _fallback_validate_candidates(obj)

    def validate_manifest(which: str, obj: Any, schema: Dict[str, Any]) -> None:
        if have_jsonschema:
            err = _validate_with_jsonschema(schema, obj)
            if err is not None:
                raise AssertionError(f"{which}: jsonschema validation failed: {err}\nInstance: {json.dumps(obj, indent=2, sort_keys=True)}")
        else:
            _fallback_validate_manifest(obj)

    with tempfile.TemporaryDirectory(prefix="qeeg_validate_biotrace_json_") as td:
        td_path = Path(td)

        # ------------------------------------------------------------------
        # Build tiny fixtures
        # ------------------------------------------------------------------
        payload_name = "exports/session_001.edf"
        payload_bytes = b"0       EDFDUMMY" + b"\x00" * 128

        container_edf = td_path / "session.edf_only.m2k"
        with zipfile.ZipFile(str(container_edf), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(payload_name, payload_bytes)

        bv_dir = "exports"
        base = "bv_session_001"
        vhdr_text, eeg_bytes, vmrk_text = _make_brainvision_triplet(base)

        container_bv = td_path / "session.brainvision.m2k"
        with zipfile.ZipFile(str(container_bv), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(f"{bv_dir}/{base}.vhdr", vhdr_text.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base}.eeg", eeg_bytes)
            zf.writestr(f"{bv_dir}/{base}.vmrk", vmrk_text.encode("utf-8"))

        # Non-zip fixture (error mode).
        not_zip = td_path / "not_a_zip.m2k"
        not_zip.write_bytes(b"not a zip")

        # ------------------------------------------------------------------
        # Validate extractor list-json (success)
        # ------------------------------------------------------------------
        for cont in (container_edf, container_bv):
            r = _run_capture(
                [sys.executable, "scripts/biotrace_extract_container.py", "--input", str(cont), "--list-json"],
                cwd=repo_root,
            )
            if r.returncode != 0:
                raise AssertionError(f"extract --list-json failed for {cont}: rc={r.returncode}\n{r.stdout}")

            obj = _json_from_stdout(r.stdout)
            validate_candidates("biotrace_extract_container.py --list-json", obj, schema_extract_list)

            if ns.verbose:
                print(f"[OK] extract list-json: {cont}")

        # ------------------------------------------------------------------
        # Validate extractor print-json (success)
        # ------------------------------------------------------------------
        outdir_edf = td_path / "out_extract_edf"
        r = _run_capture(
            [
                sys.executable,
                "scripts/biotrace_extract_container.py",
                "--input",
                str(container_edf),
                "--outdir",
                str(outdir_edf),
                "--print-json",
            ],
            cwd=repo_root,
        )
        if r.returncode != 0:
            raise AssertionError(f"extract --print-json failed (EDF): rc={r.returncode}\n{r.stdout}")

        obj = _json_from_stdout(r.stdout)
        validate_manifest("biotrace_extract_container.py --print-json", obj, schema_extract_manifest)

        main_path = Path(obj.get("main", "")) if obj.get("main") else None
        if main_path is None or not main_path.exists():
            raise AssertionError(f"manifest main missing on disk: {main_path}\n{r.stdout}")

        # ------------------------------------------------------------------
        # Validate extractor list-json (error)
        # ------------------------------------------------------------------
        r = _run_capture(
            [sys.executable, "scripts/biotrace_extract_container.py", "--input", str(not_zip), "--list-json"],
            cwd=repo_root,
        )
        if r.returncode == 0:
            raise AssertionError("expected non-zero exit for non-zip in extractor list-json")
        obj = _json_from_stdout(r.stdout)
        validate_candidates("biotrace_extract_container.py --list-json (error)", obj, schema_extract_list)
        if not obj.get("error"):
            raise AssertionError("expected error field in extractor list-json error mode")

        # ------------------------------------------------------------------
        # Validate extractor print-json (error)
        # ------------------------------------------------------------------
        r = _run_capture(
            [
                sys.executable,
                "scripts/biotrace_extract_container.py",
                "--input",
                str(not_zip),
                "--outdir",
                str(td_path / "out_extract_error"),
                "--print-json",
            ],
            cwd=repo_root,
        )
        if r.returncode == 0:
            raise AssertionError("expected non-zero exit for non-zip in extractor print-json")
        obj = _json_from_stdout(r.stdout)
        validate_manifest("biotrace_extract_container.py --print-json (error)", obj, schema_extract_manifest)
        if obj.get("main") is not None:
            raise AssertionError("expected main=null in extractor manifest error mode")
        if not obj.get("error"):
            raise AssertionError("expected error field in extractor manifest error mode")

        # ------------------------------------------------------------------
        # Validate wrapper list-json (success + error)
        # ------------------------------------------------------------------
        for cont in (container_edf, container_bv):
            r = _run_capture(
                [sys.executable, "scripts/biotrace_run_nf.py", "--container", str(cont), "--list-json"],
                cwd=repo_root,
            )
            if r.returncode != 0:
                raise AssertionError(f"run_nf --list-json failed for {cont}: rc={r.returncode}\n{r.stdout}")
            obj = _json_from_stdout(r.stdout)
            validate_candidates("biotrace_run_nf.py --list-json", obj, schema_run_nf_list)

        r = _run_capture(
            [sys.executable, "scripts/biotrace_run_nf.py", "--container", str(not_zip), "--list-json"],
            cwd=repo_root,
        )
        if r.returncode == 0:
            raise AssertionError("expected non-zero exit for non-zip in wrapper list-json")
        obj = _json_from_stdout(r.stdout)
        validate_candidates("biotrace_run_nf.py --list-json (error)", obj, schema_run_nf_list)
        if not obj.get("error"):
            raise AssertionError("expected error field in wrapper list-json error mode")

    print("validate_biotrace_json_outputs: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
