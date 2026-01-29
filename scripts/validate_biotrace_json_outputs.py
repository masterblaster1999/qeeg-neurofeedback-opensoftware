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


def _run_capture_separate(cmd: List[str], *, cwd: Path, env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    """Run a command capturing stdout/stderr separately.

    Useful when validating tools that intentionally keep stdout machine-readable and send logs to stderr.
    """
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
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


def _fallback_validate_run_manifest(obj: Any) -> None:
    if not isinstance(obj, dict):
        raise AssertionError("run manifest must be an object")

    # Minimal shape checks (used when jsonschema isn't available).
    required = [
        "schema_version",
        "started_utc",
        "finished_utc",
        "input",
        "outdir",
        "extract_dir",
        "kept_extracted",
        "prefer_exts",
        "select",
        "extracted",
        "qeeg_nf_cli",
        "cmd",
        "forwarded_args",
        "returncode",
        "dashboard",
        "error",
    ]
    for k in required:
        if k not in obj:
            raise AssertionError(f"missing required key: {k}")

    if not isinstance(obj.get("schema_version"), int) or int(obj["schema_version"]) < 1:
        raise AssertionError("schema_version must be an integer >= 1")
    if not isinstance(obj.get("started_utc"), (int, float)):
        raise AssertionError("started_utc must be a number")
    if not isinstance(obj.get("finished_utc"), (int, float)):
        raise AssertionError("finished_utc must be a number")
    if not isinstance(obj.get("input"), str) or not obj["input"]:
        raise AssertionError("input must be a non-empty string")
    if not isinstance(obj.get("outdir"), str):
        raise AssertionError("outdir must be a string")
    if not isinstance(obj.get("extract_dir"), str):
        raise AssertionError("extract_dir must be a string")
    if not isinstance(obj.get("kept_extracted"), bool):
        raise AssertionError("kept_extracted must be a boolean")
    if not _is_str_list(obj.get("prefer_exts")):
        raise AssertionError("prefer_exts must be a list of strings")
    if not isinstance(obj.get("select"), str):
        raise AssertionError("select must be a string")
    if obj.get("extracted") is not None and not isinstance(obj.get("extracted"), str):
        raise AssertionError("extracted must be a string or null")
    if obj.get("qeeg_nf_cli") is not None and not isinstance(obj.get("qeeg_nf_cli"), str):
        raise AssertionError("qeeg_nf_cli must be a string or null")
    if not _is_str_list(obj.get("cmd")):
        raise AssertionError("cmd must be a list of strings")
    if not _is_str_list(obj.get("forwarded_args")):
        raise AssertionError("forwarded_args must be a list of strings")
    if not isinstance(obj.get("returncode"), int) or int(obj["returncode"]) < 0:
        raise AssertionError("returncode must be an integer >= 0")

    dash = obj.get("dashboard")
    if not isinstance(dash, dict):
        raise AssertionError("dashboard must be an object")
    for k in ("enabled", "cmd", "pid", "info", "error", "raw_line", "terminated", "kept"):
        if k not in dash:
            raise AssertionError(f"dashboard missing key: {k}")
    if not isinstance(dash.get("enabled"), bool):
        raise AssertionError("dashboard.enabled must be a bool")
    if not _is_str_list(dash.get("cmd")):
        raise AssertionError("dashboard.cmd must be a list of strings")
    if dash.get("pid") is not None and not isinstance(dash.get("pid"), int):
        raise AssertionError("dashboard.pid must be an int or null")
    if dash.get("info") is not None and not isinstance(dash.get("info"), dict):
        raise AssertionError("dashboard.info must be an object or null")
    if dash.get("error") is not None and not isinstance(dash.get("error"), str):
        raise AssertionError("dashboard.error must be a string or null")
    if dash.get("raw_line") is not None and not isinstance(dash.get("raw_line"), str):
        raise AssertionError("dashboard.raw_line must be a string or null")
    if not isinstance(dash.get("terminated"), bool):
        raise AssertionError("dashboard.terminated must be a bool")
    if not isinstance(dash.get("kept"), bool):
        raise AssertionError("dashboard.kept must be a bool")

    if obj.get("error") is not None and not isinstance(obj.get("error"), str):
        raise AssertionError("error must be a string or null")


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


def _make_fake_nf_cli(dir_path: Path) -> Path:
    """Create a minimal qeeg_nf_cli stand-in that accepts --input/--outdir and exits 0."""

    dir_path.mkdir(parents=True, exist_ok=True)

    impl = dir_path / "qeeg_nf_cli_fake.py"
    impl.write_text(
        """#!/usr/bin/env python3
import argparse
import os
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--input', required=True)
ap.add_argument('--outdir', required=True)
# Accept and ignore the rest (matches the wrapper's pass-through behavior).
args, _ = ap.parse_known_args()

inp = Path(args.input)
out = Path(args.outdir)
if not inp.exists():
    print(f'fake qeeg_nf_cli: input does not exist: {inp}', file=sys.stderr)
    sys.exit(2)
out.mkdir(parents=True, exist_ok=True)
# Touch a file to prove we ran.
try:
    (out / 'fake_nf_cli_ran.txt').write_text('ok', encoding='utf-8')
except Exception:
    pass
sys.exit(0)
""",
        encoding="utf-8",
    )

    if os.name == "nt":
        # Windows: create a .cmd wrapper that calls the python impl.
        cmd = dir_path / "qeeg_nf_cli_fake.cmd"
        cmd.write_text(f"@echo off\r\n\"{sys.executable}\" -B \"{impl}\" %*\r\n", encoding="utf-8")
        return cmd

    # POSIX: make the python file executable.
    try:
        os.chmod(str(impl), 0o755)
    except Exception:
        pass
    return impl


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
    schema_run_nf_run = _load_schema(schemas_dir / "biotrace_run_nf_run.schema.json")

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


    def validate_run_manifest(which: str, obj: Any, schema: Dict[str, Any]) -> None:
        if have_jsonschema:
            err = _validate_with_jsonschema(schema, obj)
            if err is not None:
                raise AssertionError(
                    f"{which}: jsonschema validation failed: {err}\nInstance: {json.dumps(obj, indent=2, sort_keys=True)}"
                )
        else:
            _fallback_validate_run_manifest(obj)

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

        # ------------------------------------------------------------------
        # Validate wrapper run-json (success + error)
        # ------------------------------------------------------------------
        fake_nf_dir = td_path / "fake_nf_cli"
        fake_nf = _make_fake_nf_cli(fake_nf_dir)

        for cont in (container_edf, container_bv):
            out_nf = td_path / f"out_runjson_{cont.stem}"
            extract_nf = td_path / f"extract_runjson_{cont.stem}"

            # keep extraction dir stable (and avoid leaving behind temp dirs on failure)
            out_nf.mkdir(parents=True, exist_ok=True)
            extract_nf.mkdir(parents=True, exist_ok=True)

            r = _run_capture_separate(
                [
                    sys.executable,
                    "scripts/biotrace_run_nf.py",
                    "--container",
                    str(cont),
                    "--outdir",
                    str(out_nf),
                    "--extract-dir",
                    str(extract_nf),
                    "--nf-cli",
                    str(fake_nf),
                    "--run-json",
                    "--metric",
                    "alpha/beta:Pz",
                    "--window",
                    "2.0",
                ],
                cwd=repo_root,
            )
            if r.returncode != 0:
                raise AssertionError(
                    f"run_nf --run-json failed for {cont}: rc={r.returncode}\n--- stdout ---\n{r.stdout}\n--- stderr ---\n{r.stderr}"
                )

            obj = _json_from_stdout(r.stdout)
            validate_run_manifest("biotrace_run_nf.py --run-json", obj, schema_run_nf_run)
            if int(obj.get("returncode", 1)) != 0:
                raise AssertionError(f"expected returncode=0 in run manifest, got {obj.get('returncode')}")
            if ns.verbose:
                print(f"[OK] run_nf run-json: {cont}")

        # Error mode: non-zip container should still return JSON on stdout.
        r = _run_capture_separate(
            [
                sys.executable,
                "scripts/biotrace_run_nf.py",
                "--container",
                str(not_zip),
                "--outdir",
                str(td_path / "out_runjson_error"),
                "--run-json",
            ],
            cwd=repo_root,
        )
        if r.returncode == 0:
            raise AssertionError("expected non-zero exit for non-zip in wrapper run-json")
        obj = _json_from_stdout(r.stdout)
        validate_run_manifest("biotrace_run_nf.py --run-json (error)", obj, schema_run_nf_run)
        if not obj.get("error"):
            raise AssertionError("expected error field in wrapper run-json error mode")

    print("validate_biotrace_json_outputs: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
