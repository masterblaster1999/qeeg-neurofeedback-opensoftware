#!/usr/bin/env python3
"""selftest_biotrace_run_nf.py

Smoke-test for scripts/biotrace_run_nf.py.

This test:
  - builds a tiny ZIP file with a dummy .edf payload (using the .m2k extension)
  - builds a tiny ZIP file with a BrainVision triplet (.vhdr/.eeg/.vmrk)
  - runs biotrace_run_nf.py in both --list and run modes
  - uses a fake qeeg_nf_cli executable to capture the forwarded arguments

No third-party dependencies.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


SCHEMA_LIST_ID = (
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/"
    "biotrace_run_nf_list.schema.json"
)


def _run(args, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "scripts/biotrace_run_nf.py"] + args,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )


def _make_fake_nf_cli(dir_path: Path) -> Path:
    """Create a tiny *executable* fake qeeg_nf_cli.

    The wrapper script (biotrace_run_nf.py) launches qeeg_nf_cli via subprocess.

    - On POSIX, we can use a shebang Python script and mark it executable.
    - On Windows, shebangs are not honored, so we generate a .cmd launcher that
      invokes the Python implementation with the current interpreter.

    The fake CLI logs argv to the JSON file pointed to by QEEG_FAKE_NF_LOG.
    """

    impl = dir_path / "qeeg_nf_cli_fake.py"

    impl.write_text(
        """#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

log_path = Path(os.environ.get('QEEG_FAKE_NF_LOG', ''))
if not log_path:
    print('QEEG_FAKE_NF_LOG not set', file=sys.stderr)
    sys.exit(2)

# Basic sanity: ensure --input exists.
argv = sys.argv[1:]
inp = None
outdir = None
for i, a in enumerate(argv):
    if a == '--input' and i + 1 < len(argv):
        inp = argv[i + 1]
    if a == '--outdir' and i + 1 < len(argv):
        outdir = argv[i + 1]

if inp is None or not Path(inp).exists():
    print(f'--input missing or not found: {inp}', file=sys.stderr)
    sys.exit(3)
if outdir is None:
    print('--outdir missing', file=sys.stderr)
    sys.exit(4)

log_path.parent.mkdir(parents=True, exist_ok=True)
log_path.write_text(json.dumps({'argv': argv}, indent=2))
sys.exit(0)
""",
        encoding="utf-8",
    )

    if os.name == "nt":
        launcher = dir_path / "qeeg_nf_cli_fake.cmd"
        # Use the *current* Python interpreter, quoted for spaces.
        # Write the .cmd file with explicit newlines (no reliance on shebang on Windows).
        launcher.write_text(
            f'@echo off\n"{sys.executable}" "{impl}" %*\n',
            encoding='utf-8',
        )
        return launcher

    # POSIX: mark the Python file executable and return it.
    os.chmod(impl, 0o755)
    return impl


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


def _read_logged_argv(log_path: Path) -> list[str]:
    data = json.loads(log_path.read_text(encoding="utf-8"))
    argv = data.get("argv", [])
    if not isinstance(argv, list):
        raise AssertionError("logged argv is not a list")
    return [str(x) for x in argv]


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory(prefix="qeeg_biotrace_run_nf_") as td:
        td_path = Path(td)

        # Prepare fake nf cli once.
        fake_nf = _make_fake_nf_cli(td_path)

        # ------------------------------------------------------------------
        # Case 1: EDF payload
        # ------------------------------------------------------------------
        container_path = td_path / "session.edf_only.m2k"
        payload_name = "exports/session_001.edf"
        payload_bytes = b"0       EDFDUMMY" + b"\x00" * 128

        with zipfile.ZipFile(str(container_path), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(payload_name, payload_bytes)

        # --list should show the payload.
        r = _run(["--container", str(container_path), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("--list failed")
        if "session_001.edf" not in r.stdout:
            print(r.stdout)
            raise AssertionError("--list did not include expected member")

        # --list-json should emit machine-readable JSON.
        rj = _run(["--container", str(container_path), "--list-json"], repo_root)
        if rj.returncode != 0:
            print(rj.stdout)
            raise AssertionError("--list-json failed")
        dj = json.loads(rj.stdout)
        if dj.get("$schema") != SCHEMA_LIST_ID:
            print(rj.stdout)
            raise AssertionError("--list-json missing/incorrect $schema")
        cands = dj.get("candidates", [])
        if not cands or "session_001.edf" not in str(cands[0].get("name", "")):
            print(rj.stdout)
            raise AssertionError("--list-json did not include expected member")

        outdir = td_path / "out_nf_edf"
        extract_dir = td_path / "extracted_edf"
        outdir.mkdir(parents=True, exist_ok=True)
        extract_dir.mkdir(parents=True, exist_ok=True)
        log_path = td_path / "nf_args_edf.json"

        env = os.environ.copy()
        env["QEEG_FAKE_NF_LOG"] = str(log_path)

        r = subprocess.run(
            [
                sys.executable,
                "scripts/biotrace_run_nf.py",
                "--container",
                str(container_path),
                "--outdir",
                str(outdir),
                "--extract-dir",
                str(extract_dir),
                "--nf-cli",
                str(fake_nf),
                "--",
                "--metric",
                "alpha/beta:Pz",
                "--window",
                "2.0",
            ],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
            check=False,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("run mode (EDF) failed")

        if not log_path.exists():
            raise AssertionError("fake nf cli did not write log (EDF)")

        # Ensure extraction happened.
        extracted_files = list(extract_dir.rglob("*.edf"))
        if not extracted_files:
            raise AssertionError("expected extracted .edf (EDF)")
        if extracted_files[0].read_bytes() != payload_bytes:
            raise AssertionError("extracted bytes mismatch (EDF)")

        # Ensure wrapper injected --input/--outdir and preserved args.
        log_argv = _read_logged_argv(log_path)
        if "--input" not in log_argv or "--outdir" not in log_argv:
            raise AssertionError("expected --input/--outdir in forwarded args (EDF)")
        if "alpha/beta:Pz" not in log_argv:
            raise AssertionError("expected passthrough args in forwarded args (EDF)")

        # Also ensure the wrapper forwards qeeg_nf_cli args even *without* a '--' separator.
        outdir_nd = td_path / "out_nf_edf_no_delim"
        extract_dir_nd = td_path / "extracted_edf_no_delim"
        outdir_nd.mkdir(parents=True, exist_ok=True)
        extract_dir_nd.mkdir(parents=True, exist_ok=True)
        log_path_nd = td_path / "nf_args_edf_no_delim.json"

        env_nd = os.environ.copy()
        env_nd["QEEG_FAKE_NF_LOG"] = str(log_path_nd)

        r = subprocess.run(
            [
                sys.executable,
                "scripts/biotrace_run_nf.py",
                "--container",
                str(container_path),
                "--outdir",
                str(outdir_nd),
                "--extract-dir",
                str(extract_dir_nd),
                "--nf-cli",
                str(fake_nf),
                "--metric",
                "alpha/beta:Pz",
                "--window",
                "2.0",
            ],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env_nd,
            check=False,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("run mode (EDF, no-delimiter) failed")

        if not log_path_nd.exists():
            raise AssertionError("fake nf cli did not write log (EDF, no-delimiter)")

        log_argv_nd = _read_logged_argv(log_path_nd)
        if "alpha/beta:Pz" not in log_argv_nd:
            raise AssertionError("expected passthrough args in forwarded args (EDF, no-delimiter)")

        # ------------------------------------------------------------------
        # Case 2: BrainVision triplet payload
        # ------------------------------------------------------------------
        bv_container = td_path / "session.brainvision.m2k"
        bv_dir = "exports"
        base = "bv_session_001"
        vhdr_text, eeg_bytes, vmrk_text = _make_brainvision_triplet(base)

        with zipfile.ZipFile(str(bv_container), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(f"{bv_dir}/{base}.vhdr", vhdr_text.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base}.eeg", eeg_bytes)
            zf.writestr(f"{bv_dir}/{base}.vmrk", vmrk_text.encode("utf-8"))

        # --list should show the .vhdr (candidate).
        r = _run(["--container", str(bv_container), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("--list failed (BrainVision)")
        if f"{base}.vhdr" not in r.stdout:
            print(r.stdout)
            raise AssertionError("--list did not include expected .vhdr member (BrainVision)")

        # --list-json should include the .vhdr candidate.
        rj = _run(["--container", str(bv_container), "--list-json"], repo_root)
        if rj.returncode != 0:
            print(rj.stdout)
            raise AssertionError("--list-json failed (BrainVision)")
        dj = json.loads(rj.stdout)
        if dj.get("$schema") != SCHEMA_LIST_ID:
            print(rj.stdout)
            raise AssertionError("--list-json missing/incorrect $schema (BrainVision)")
        names = [str(x.get("name", "")) for x in dj.get("candidates", [])]
        if not any(f"{base}.vhdr" in n for n in names):
            print(rj.stdout)
            raise AssertionError("--list-json missing expected .vhdr member (BrainVision)")

        outdir_bv = td_path / "out_nf_bv"
        extract_dir_bv = td_path / "extracted_bv"
        outdir_bv.mkdir(parents=True, exist_ok=True)
        extract_dir_bv.mkdir(parents=True, exist_ok=True)
        log_path_bv = td_path / "nf_args_bv.json"

        env_bv = os.environ.copy()
        env_bv["QEEG_FAKE_NF_LOG"] = str(log_path_bv)

        r = subprocess.run(
            [
                sys.executable,
                "scripts/biotrace_run_nf.py",
                "--container",
                str(bv_container),
                "--outdir",
                str(outdir_bv),
                "--extract-dir",
                str(extract_dir_bv),
                "--nf-cli",
                str(fake_nf),
                "--",
                "--metric",
                "theta/beta:Cz",
                "--window",
                "1.0",
            ],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env_bv,
            check=False,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("run mode (BrainVision) failed")

        if not log_path_bv.exists():
            raise AssertionError("fake nf cli did not write log (BrainVision)")

        # Ensure the triplet was extracted.
        exp_vhdr = extract_dir_bv / bv_dir / f"{base}.vhdr"
        exp_eeg = extract_dir_bv / bv_dir / f"{base}.eeg"
        exp_vmrk = extract_dir_bv / bv_dir / f"{base}.vmrk"

        for p in (exp_vhdr, exp_eeg, exp_vmrk):
            if not p.exists():
                raise AssertionError(f"expected extracted BrainVision file missing: {p}")

        if exp_eeg.read_bytes() != eeg_bytes:
            raise AssertionError("extracted bytes mismatch (BrainVision .eeg)")

        log_argv_bv = _read_logged_argv(log_path_bv)
        if "theta/beta:Cz" not in log_argv_bv:
            raise AssertionError("expected passthrough args in forwarded args (BrainVision)")

        # Confirm wrapper passed the .vhdr as --input.
        inp = None
        for i, a in enumerate(log_argv_bv):
            if a == "--input" and i + 1 < len(log_argv_bv):
                inp = log_argv_bv[i + 1]
        if inp is None or not inp.endswith(".vhdr"):
            raise AssertionError(f"expected --input to be .vhdr, got: {inp}")


        # ------------------------------------------------------------------
        # Case 2b: BrainVision header stored as .txt (extractor should rename to .vhdr)
        # ------------------------------------------------------------------
        bv_container_txt = td_path / "session.brainvision_txt.m2k"
        base_txt = "bv_session_txt"
        vhdr_text_t, eeg_bytes_t, vmrk_text_t = _make_brainvision_triplet(base_txt)

        with zipfile.ZipFile(str(bv_container_txt), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(f"{bv_dir}/{base_txt}.txt", vhdr_text_t.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base_txt}.eeg", eeg_bytes_t)
            zf.writestr(f"{bv_dir}/{base_txt}.vmrk", vmrk_text_t.encode("utf-8"))

        # --list should include the .txt member.
        r = _run(["--container", str(bv_container_txt), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("--list failed (BrainVision .txt)")
        if f"{base_txt}.txt" not in r.stdout:
            print(r.stdout)
            raise AssertionError("--list did not include expected .txt member (BrainVision)")

        outdir_bv_t = td_path / "out_nf_bv_txt"
        extract_dir_bv_t = td_path / "extracted_bv_txt"
        outdir_bv_t.mkdir(parents=True, exist_ok=True)
        extract_dir_bv_t.mkdir(parents=True, exist_ok=True)
        log_path_bv_t = td_path / "nf_args_bv_txt.json"

        env_bv_t = os.environ.copy()
        env_bv_t["QEEG_FAKE_NF_LOG"] = str(log_path_bv_t)

        r = subprocess.run(
            [
                sys.executable,
                "scripts/biotrace_run_nf.py",
                "--container",
                str(bv_container_txt),
                "--outdir",
                str(outdir_bv_t),
                "--extract-dir",
                str(extract_dir_bv_t),
                "--nf-cli",
                str(fake_nf),
                "--",
                "--metric",
                "alpha:Pz",
                "--window",
                "1.0",
            ],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env_bv_t,
            check=False,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("run mode (BrainVision .txt) failed")

        log_argv_bv_t = _read_logged_argv(log_path_bv_t)
        inp_t = None
        for i, a in enumerate(log_argv_bv_t):
            if a == "--input" and i + 1 < len(log_argv_bv_t):
                inp_t = log_argv_bv_t[i + 1]
        if inp_t is None or not inp_t.endswith(".vhdr"):
            raise AssertionError(f"expected --input to be renamed .vhdr, got: {inp_t}")

        # Ensure renamed header exists.
        exp_vhdr_t = extract_dir_bv_t / bv_dir / f"{base_txt}.vhdr"
        exp_eeg_t = extract_dir_bv_t / bv_dir / f"{base_txt}.eeg"
        exp_vmrk_t = extract_dir_bv_t / bv_dir / f"{base_txt}.vmrk"
        for p in (exp_vhdr_t, exp_eeg_t, exp_vmrk_t):
            if not p.exists():
                raise AssertionError(f"expected extracted BrainVision file missing (.txt case): {p}")


        # ------------------------------------------------------------------
        # Case 2c: BrainVision header stored as UTF-16LE .txt (no BOM)
        # ------------------------------------------------------------------
        bv_container_txt_u16 = td_path / "session.brainvision_txt_utf16le.m2k"
        base_txt_u16 = "bv_session_txt_u16"
        vhdr_text_u16, eeg_bytes_u16, vmrk_text_u16 = _make_brainvision_triplet(base_txt_u16)

        with zipfile.ZipFile(str(bv_container_txt_u16), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(f"{bv_dir}/{base_txt_u16}.txt", vhdr_text_u16.encode("utf-16-le"))
            zf.writestr(f"{bv_dir}/{base_txt_u16}.eeg", eeg_bytes_u16)
            zf.writestr(f"{bv_dir}/{base_txt_u16}.vmrk", vmrk_text_u16.encode("utf-16-le"))

        outdir_bv_u16 = td_path / "out_nf_bv_txt_u16"
        extract_dir_bv_u16 = td_path / "extracted_bv_txt_u16"
        outdir_bv_u16.mkdir(parents=True, exist_ok=True)
        extract_dir_bv_u16.mkdir(parents=True, exist_ok=True)
        log_path_bv_u16 = td_path / "nf_args_bv_txt_u16.json"

        env_bv_u16 = os.environ.copy()
        env_bv_u16["QEEG_FAKE_NF_LOG"] = str(log_path_bv_u16)

        r = subprocess.run(
            [
                sys.executable,
                "scripts/biotrace_run_nf.py",
                "--container",
                str(bv_container_txt_u16),
                "--outdir",
                str(outdir_bv_u16),
                "--extract-dir",
                str(extract_dir_bv_u16),
                "--nf-cli",
                str(fake_nf),
                "--",
                "--metric",
                "alpha:Pz",
                "--window",
                "1.0",
            ],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env_bv_u16,
            check=False,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("run mode (BrainVision UTF-16LE .txt) failed")

        log_argv_bv_u16 = _read_logged_argv(log_path_bv_u16)
        inp_u16 = None
        for i, a in enumerate(log_argv_bv_u16):
            if a == "--input" and i + 1 < len(log_argv_bv_u16):
                inp_u16 = log_argv_bv_u16[i + 1]
        if inp_u16 is None or not inp_u16.endswith(".vhdr"):
            raise AssertionError(f"expected --input to be renamed .vhdr (UTF-16LE case), got: {inp_u16}")

        exp_vhdr_u16 = extract_dir_bv_u16 / bv_dir / f"{base_txt_u16}.vhdr"
        if not exp_vhdr_u16.exists():
            raise AssertionError(f"expected extracted UTF-16LE .vhdr missing: {exp_vhdr_u16}")

        # Encoding sanity: expect NUL bytes (UTF-16).
        if exp_vhdr_u16.read_bytes().count(b"\x00") < 10:
            raise AssertionError("expected extracted UTF-16LE .vhdr to contain NUL bytes")


        # ------------------------------------------------------------------
        # Case 3: Multi-export container + --select
        # ------------------------------------------------------------------
        multi_container = td_path / "session.multi.m2k"
        with zipfile.ZipFile(str(multi_container), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(payload_name, payload_bytes)
            zf.writestr(f"{bv_dir}/{base}.vhdr", vhdr_text.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base}.eeg", eeg_bytes)
            zf.writestr(f"{bv_dir}/{base}.vmrk", vmrk_text.encode("utf-8"))

        outdir_multi = td_path / "out_nf_multi"
        extract_dir_multi = td_path / "extracted_multi"
        outdir_multi.mkdir(parents=True, exist_ok=True)
        extract_dir_multi.mkdir(parents=True, exist_ok=True)
        log_path_multi = td_path / "nf_args_multi.json"

        env_multi = os.environ.copy()
        env_multi["QEEG_FAKE_NF_LOG"] = str(log_path_multi)

        r = subprocess.run(
            [
                sys.executable,
                "scripts/biotrace_run_nf.py",
                "--container",
                str(multi_container),
                "--outdir",
                str(outdir_multi),
                "--extract-dir",
                str(extract_dir_multi),
                "--nf-cli",
                str(fake_nf),
                "--select",
                "2",
                "--",
                "--metric",
                "alpha:Pz",
                "--window",
                "1.0",
            ],
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env_multi,
            check=False,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("run mode (multi --select) failed")

        log_argv_multi = _read_logged_argv(log_path_multi)
        inp_multi = None
        for i, a in enumerate(log_argv_multi):
            if a == "--input" and i + 1 < len(log_argv_multi):
                inp_multi = log_argv_multi[i + 1]
        if inp_multi is None or not inp_multi.endswith(".vhdr"):
            raise AssertionError(f"expected --select to choose BrainVision .vhdr, got: {inp_multi}")

        # Ensure triplet was extracted under extract_dir_multi.
        for p in (
            extract_dir_multi / bv_dir / f"{base}.vhdr",
            extract_dir_multi / bv_dir / f"{base}.eeg",
            extract_dir_multi / bv_dir / f"{base}.vmrk",
        ):
            if not p.exists():
                raise AssertionError(f"expected extracted file missing (multi select): {p}")

    print("selftest_biotrace_run_nf: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
