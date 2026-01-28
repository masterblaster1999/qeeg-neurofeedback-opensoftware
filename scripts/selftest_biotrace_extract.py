#!/usr/bin/env python3
"""selftest_biotrace_extract.py

Smoke-test for scripts/biotrace_extract_container.py.

This test:
  - builds a tiny ZIP file with a dummy .edf payload (using the .m2k extension)
  - builds a tiny ZIP file with a BrainVision triplet (.vhdr/.eeg/.vmrk)
  - runs the extractor in both --list and extract modes
  - verifies extraction output
  - verifies ZipSlip protection (refuses unsafe paths)

No third-party dependencies.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


SCHEMA_LIST_ID = (
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/"
    "biotrace_extract_list.schema.json"
)
SCHEMA_MANIFEST_ID = (
    "https://raw.githubusercontent.com/masterblaster1999/qeeg-neurofeedback-opensoftware/main/schemas/"
    "biotrace_extract_manifest.schema.json"
)


def _run(args, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "scripts/biotrace_extract_container.py"] + args,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )


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


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory(prefix="qeeg_biotrace_extract_") as td:
        td_path = Path(td)

        # 1) Dummy EDF payload in a .m2k (ZIP)
        container_path = td_path / "session.edf_only.m2k"
        payload_name = "exports/session_001.edf"
        payload_bytes = b"0       EDFDUMMY" + b"\x00" * 128

        with zipfile.ZipFile(str(container_path), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(payload_name, payload_bytes)

        # --list should show the payload.
        r = _run(["--input", str(container_path), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("--list failed")
        if "session_001.edf" not in r.stdout:
            print(r.stdout)
            raise AssertionError("--list did not include expected member")

        # --list-json should be machine-readable and include the payload.
        rj = _run(["--input", str(container_path), "--list-json"], repo_root)
        if rj.returncode != 0:
            print(rj.stdout)
            raise AssertionError("--list-json failed")
        data = json.loads(rj.stdout)
        if data.get("$schema") != SCHEMA_LIST_ID:
            print(rj.stdout)
            raise AssertionError("--list-json missing/incorrect $schema")
        cands = data.get("candidates", [])
        if not cands or "session_001.edf" not in str(cands[0].get("name", "")):
            print(rj.stdout)
            raise AssertionError("--list-json did not include expected member")

        # --container is an alias for --input.
        r2 = _run(["--container", str(container_path), "--list"], repo_root)
        if r2.returncode != 0:
            print(r2.stdout)
            raise AssertionError("--container alias list failed")
        if "session_001.edf" not in r2.stdout:
            print(r2.stdout)
            raise AssertionError("--container alias list missing expected member")

        # Extract the best payload.
        outdir = td_path / "out_edf"
        r = _run(["--container", str(container_path), "--outdir", str(outdir), "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("extract failed")
        extracted_path = Path(r.stdout.strip().splitlines()[-1])
        if not extracted_path.exists():
            raise AssertionError(f"extracted file missing: {extracted_path}")
        if extracted_path.read_bytes() != payload_bytes:
            raise AssertionError("extracted bytes mismatch")

        # Extraction with --print-json should also work and return JSON.
        outdir_json = td_path / "out_edf_json"
        r = _run(["--container", str(container_path), "--outdir", str(outdir_json), "--print-json"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("extract --print-json failed")
        d = json.loads(r.stdout)
        if d.get("$schema") != SCHEMA_MANIFEST_ID:
            print(r.stdout)
            raise AssertionError("--print-json missing/incorrect $schema")
        main_p = Path(d.get("main", ""))
        extracted_ps = [Path(x) for x in d.get("extracted", [])]
        if not main_p.exists() or main_p not in extracted_ps:
            raise AssertionError("--print-json did not include a valid main path")
        if main_p.read_bytes() != payload_bytes:
            raise AssertionError("--print-json extracted bytes mismatch")

        # 2) BrainVision triplet in a .m2k (ZIP)
        bv_container = td_path / "session.brainvision.m2k"
        bv_dir = "exports"
        base = "bv_session_001"
        vhdr_text, eeg_bytes, vmrk_text = _make_brainvision_triplet(base)

        with zipfile.ZipFile(str(bv_container), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(f"{bv_dir}/{base}.vhdr", vhdr_text.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base}.eeg", eeg_bytes)
            zf.writestr(f"{bv_dir}/{base}.vmrk", vmrk_text.encode("utf-8"))

        # --list should show the .vhdr (candidate).
        r = _run(["--input", str(bv_container), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision --list failed")
        if f"{base}.vhdr" not in r.stdout:
            print(r.stdout)
            raise AssertionError("BrainVision --list did not include expected .vhdr member")

        # --list-json should include the .vhdr candidate.
        rj = _run(["--input", str(bv_container), "--list-json"], repo_root)
        if rj.returncode != 0:
            print(rj.stdout)
            raise AssertionError("BrainVision --list-json failed")
        dj = json.loads(rj.stdout)
        if dj.get("$schema") != SCHEMA_LIST_ID:
            print(rj.stdout)
            raise AssertionError("BrainVision --list-json missing/incorrect $schema")
        cands = dj.get("candidates", [])
        names = [str(x.get("name", "")) for x in cands]
        if not any(f"{base}.vhdr" in n for n in names):
            print(rj.stdout)
            raise AssertionError("BrainVision --list-json missing expected .vhdr member")

        # Extract should include the triplet.
        outdir_bv = td_path / "out_bv"
        r = _run(["--container", str(bv_container), "--outdir", str(outdir_bv), "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision extract failed")

        extracted_lines = [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]
        extracted_paths = [Path(x) for x in extracted_lines]

        # Expect at least 3 files: vhdr + eeg + vmrk
        if len(extracted_paths) < 3:
            print(r.stdout)
            raise AssertionError(f"expected >=3 extracted files, got {len(extracted_paths)}")

        # Ensure all expected files exist and match.
        exp_vhdr = outdir_bv / bv_dir / f"{base}.vhdr"
        exp_eeg = outdir_bv / bv_dir / f"{base}.eeg"
        exp_vmrk = outdir_bv / bv_dir / f"{base}.vmrk"

        for p in (exp_vhdr, exp_eeg, exp_vmrk):
            if not p.exists():
                raise AssertionError(f"expected extracted BrainVision file missing: {p}")

        if exp_vhdr.read_text(encoding="utf-8") != vhdr_text:
            raise AssertionError("BrainVision .vhdr text mismatch")
        if exp_eeg.read_bytes() != eeg_bytes:
            raise AssertionError("BrainVision .eeg bytes mismatch")
        if exp_vmrk.read_text(encoding="utf-8") != vmrk_text:
            raise AssertionError("BrainVision .vmrk text mismatch")

        # Also verify --print-json returns the expected multi-file extraction.
        outdir_bv_json = td_path / "out_bv_json"
        rj = _run(["--container", str(bv_container), "--outdir", str(outdir_bv_json), "--print-json"], repo_root)
        if rj.returncode != 0:
            print(rj.stdout)
            raise AssertionError("BrainVision extract --print-json failed")
        dj = json.loads(rj.stdout)
        if dj.get("$schema") != SCHEMA_MANIFEST_ID:
            print(rj.stdout)
            raise AssertionError("BrainVision --print-json missing/incorrect $schema")
        main_bv = Path(dj.get("main", ""))
        extracted_bv = [Path(x) for x in dj.get("extracted", [])]
        if not main_bv.name.endswith(".vhdr"):
            raise AssertionError("BrainVision --print-json main should be a .vhdr")
        if len(extracted_bv) < 3:
            raise AssertionError("BrainVision --print-json expected >=3 extracted paths")
        # Ensure the extracted files exist.
        for p in extracted_bv:
            if not p.exists():
                raise AssertionError(f"BrainVision --print-json extracted file missing: {p}")

        
        # 2c) BrainVision triplet with mismatched casing (vhdr references upper-case, members are lower-case).
        bv_container_case = td_path / "session.brainvision.case.m2k"
        base2 = "bv_session_002"

        vhdr_text2 = (
            "Brain Vision Data Exchange Header File Version 1.0\n"
            "[Common Infos]\n"
            f"DataFile={base2.upper()}.EEG\n"
            f"MarkerFile={base2.upper()}.VMRK\n"
            "DataFormat=BINARY\n"
        )
        eeg_bytes2 = b"EEGDUMMY2" + b"\x05\x06\x07\x08" * 16
        vmrk_text2 = (
            "Brain Vision Data Exchange Marker File Version 1.0\n"
            "[Common Infos]\n"
            f"DataFile={base2.upper()}.EEG\n"
            "[Marker Infos]\n"
            "Mk1=Stimulus,S 1,1,1,0\n"
        )

        with zipfile.ZipFile(str(bv_container_case), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr(f"{bv_dir}/{base2}.vhdr", vhdr_text2.encode("utf-8"))
            # Store the companion files in lower-case (common after extraction on some systems).
            zf.writestr(f"{bv_dir}/{base2}.eeg", eeg_bytes2)
            zf.writestr(f"{bv_dir}/{base2}.vmrk", vmrk_text2.encode("utf-8"))

        outdir_bv_case = td_path / "out_bv_case"
        r = _run(["--container", str(bv_container_case), "--outdir", str(outdir_bv_case), "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision case-mismatch extract failed")

        exp_vhdr_case = outdir_bv_case / bv_dir / f"{base2}.vhdr"
        exp_eeg_case = outdir_bv_case / bv_dir / f"{base2}.eeg"
        exp_vmrk_case = outdir_bv_case / bv_dir / f"{base2}.vmrk"

        for p in (exp_vhdr_case, exp_eeg_case, exp_vmrk_case):
            if not p.exists():
                raise AssertionError(f"expected extracted BrainVision file missing (case test): {p}")

        vhdr_case_out = exp_vhdr_case.read_text(encoding="utf-8", errors="ignore")
        if (f"DataFile={base2}.eeg" not in vhdr_case_out) and (f"DataFile = {base2}.eeg" not in vhdr_case_out):
            print(vhdr_case_out)
            raise AssertionError("expected extractor to patch vhdr DataFile to match extracted .eeg case")
        if (f"MarkerFile={base2}.vmrk" not in vhdr_case_out) and (f"MarkerFile = {base2}.vmrk" not in vhdr_case_out):
            print(vhdr_case_out)
            raise AssertionError("expected extractor to patch vhdr MarkerFile to match extracted .vmrk case")

        vmrk_case_out = exp_vmrk_case.read_text(encoding="utf-8", errors="ignore")
        if (f"DataFile={base2}.eeg" not in vmrk_case_out) and (f"DataFile = {base2}.eeg" not in vmrk_case_out):
            print(vmrk_case_out)
            raise AssertionError("expected extractor to patch vmrk DataFile to match extracted .eeg case")

# 2b) Multi-export container: EDF + BrainVision, test --select.
        multi_container = td_path / "session.multi.m2k"
        with zipfile.ZipFile(str(multi_container), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("exports/session_001.edf", payload_bytes)
            zf.writestr(f"{bv_dir}/{base}.vhdr", vhdr_text.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base}.eeg", eeg_bytes)
            zf.writestr(f"{bv_dir}/{base}.vmrk", vmrk_text.encode("utf-8"))

        # --list should include both candidates and show indices.
        r = _run(["--input", str(multi_container), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("multi --list failed")
        if "session_001.edf" not in r.stdout or f"{base}.vhdr" not in r.stdout:
            print(r.stdout)
            raise AssertionError("multi --list missing expected members")

        # Default extraction should pick EDF (preferred by DEFAULT_PREFER).
        outdir_multi = td_path / "out_multi_default"
        r = _run(["--input", str(multi_container), "--outdir", str(outdir_multi), "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("multi extract default failed")
        extracted_last = Path(r.stdout.strip().splitlines()[-1])
        if not extracted_last.name.endswith(".edf"):
            raise AssertionError(f"expected default extraction to pick .edf, got: {extracted_last}")

        # --select 2 should pick the BrainVision .vhdr (2nd in ranked list).
        outdir_sel = td_path / "out_multi_select"
        r = _run(
            [
                "--input",
                str(multi_container),
                "--outdir",
                str(outdir_sel),
                "--select",
                "2",
                "--print",
            ],
            repo_root,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("multi extract --select 2 failed")
        # Ensure triplet exists.
        exp_vhdr2 = outdir_sel / bv_dir / f"{base}.vhdr"
        exp_eeg2 = outdir_sel / bv_dir / f"{base}.eeg"
        exp_vmrk2 = outdir_sel / bv_dir / f"{base}.vmrk"
        for p in (exp_vhdr2, exp_eeg2, exp_vmrk2):
            if not p.exists():
                raise AssertionError(f"expected extracted file missing (select): {p}")

        # --select with a glob pattern should also work (previously could crash).
        outdir_glob = td_path / "out_multi_glob"
        r = _run(
            [
                "--input",
                str(multi_container),
                "--outdir",
                str(outdir_glob),
                "--select",
                "*.vhdr",
                "--print",
            ],
            repo_root,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("multi extract --select glob failed")
        exp_vhdr_g = outdir_glob / bv_dir / f"{base}.vhdr"
        exp_eeg_g = outdir_glob / bv_dir / f"{base}.eeg"
        exp_vmrk_g = outdir_glob / bv_dir / f"{base}.vmrk"
        for p in (exp_vhdr_g, exp_eeg_g, exp_vmrk_g):
            if not p.exists():
                raise AssertionError(f"expected extracted file missing (glob select): {p}")

        # --select by substring should be unambiguous and work.
        outdir_sub = td_path / "out_multi_sub"
        r = _run(
            [
                "--input",
                str(multi_container),
                "--outdir",
                str(outdir_sub),
                "--select",
                base,
                "--print",
            ],
            repo_root,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("multi extract --select substring failed")
        exp_vhdr_s = outdir_sub / bv_dir / f"{base}.vhdr"
        exp_eeg_s = outdir_sub / bv_dir / f"{base}.eeg"
        exp_vmrk_s = outdir_sub / bv_dir / f"{base}.vmrk"
        for p in (exp_vhdr_s, exp_eeg_s, exp_vmrk_s):
            if not p.exists():
                raise AssertionError(f"expected extracted file missing (substring select): {p}")


        # 2d) BrainVision header stored as .txt (some exporters use .txt for .vhdr).
        bv_container_txt = td_path / "session.brainvision_txt.m2k"
        base3 = "bv_session_003"
        vhdr_text3, eeg_bytes3, vmrk_text3 = _make_brainvision_triplet(base3)

        with zipfile.ZipFile(str(bv_container_txt), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            # Store the header as .txt, but with BrainVision header contents.
            zf.writestr(f"{bv_dir}/{base3}.txt", vhdr_text3.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base3}.eeg", eeg_bytes3)
            zf.writestr(f"{bv_dir}/{base3}.vmrk", vmrk_text3.encode("utf-8"))

        # --list should include the .txt member.
        r = _run(["--input", str(bv_container_txt), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision .txt --list failed")
        if f"{base3}.txt" not in r.stdout:
            print(r.stdout)
            raise AssertionError("BrainVision .txt --list missing expected member")

        # Default extract should rename the header to .vhdr and extract the triplet.
        outdir_bv_txt = td_path / "out_bv_txt"
        r = _run(["--container", str(bv_container_txt), "--outdir", str(outdir_bv_txt), "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision .txt extract failed")

        exp_vhdr_txt = outdir_bv_txt / bv_dir / f"{base3}.vhdr"
        exp_eeg_txt = outdir_bv_txt / bv_dir / f"{base3}.eeg"
        exp_vmrk_txt = outdir_bv_txt / bv_dir / f"{base3}.vmrk"

        for p in (exp_vhdr_txt, exp_eeg_txt, exp_vmrk_txt):
            if not p.exists():
                raise AssertionError(f"expected extracted BrainVision file missing (.txt header test): {p}")

        # Ensure the original .txt header name was converted to .vhdr.
        if (outdir_bv_txt / bv_dir / f"{base3}.txt").exists():
            raise AssertionError("expected extractor to rename BrainVision .txt header to .vhdr")

        if exp_eeg_txt.read_bytes() != eeg_bytes3:
            raise AssertionError("BrainVision .txt header: .eeg bytes mismatch")

        # --select '*.vhdr' should match by effective extension even though the container member was .txt.
        outdir_bv_txt_sel = td_path / "out_bv_txt_sel"
        r = _run(["--input", str(bv_container_txt), "--outdir", str(outdir_bv_txt_sel), "--select", "*.vhdr", "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision .txt header: --select *.vhdr failed")
        if not (outdir_bv_txt_sel / bv_dir / f"{base3}.vhdr").exists():
            raise AssertionError("BrainVision .txt header: --select *.vhdr did not extract .vhdr")


        # 2d2) BrainVision header stored as .txt but encoded as UTF-16LE without BOM ("Unicode" text).
        # The extractor should still detect it as BrainVision, rename to .vhdr, and extract the triplet.
        bv_container_txt_u16 = td_path / "session.brainvision_txt_utf16le.m2k"
        base3u = "bv_session_003u16"
        vhdr_text3u, eeg_bytes3u, vmrk_text3u = _make_brainvision_triplet(base3u)

        with zipfile.ZipFile(str(bv_container_txt_u16), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            # Store the header and marker as UTF-16LE (no BOM) and the data as binary.
            zf.writestr(f"{bv_dir}/{base3u}.txt", vhdr_text3u.encode("utf-16-le"))
            zf.writestr(f"{bv_dir}/{base3u}.eeg", eeg_bytes3u)
            zf.writestr(f"{bv_dir}/{base3u}.vmrk", vmrk_text3u.encode("utf-16-le"))

        # --list should include the .txt member.
        r = _run(["--input", str(bv_container_txt_u16), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision UTF-16LE .txt --list failed")
        if f"{base3u}.txt" not in r.stdout:
            print(r.stdout)
            raise AssertionError("BrainVision UTF-16LE .txt --list missing expected member")

        outdir_bv_txt_u16 = td_path / "out_bv_txt_u16"
        r = _run(
            ["--container", str(bv_container_txt_u16), "--outdir", str(outdir_bv_txt_u16), "--print"],
            repo_root,
        )
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision UTF-16LE .txt extract failed")

        exp_vhdr_u16 = outdir_bv_txt_u16 / bv_dir / f"{base3u}.vhdr"
        exp_eeg_u16 = outdir_bv_txt_u16 / bv_dir / f"{base3u}.eeg"
        exp_vmrk_u16 = outdir_bv_txt_u16 / bv_dir / f"{base3u}.vmrk"

        for p in (exp_vhdr_u16, exp_eeg_u16, exp_vmrk_u16):
            if not p.exists():
                raise AssertionError(f"expected extracted BrainVision file missing (UTF-16LE .txt): {p}")

        # Header should have been renamed to .vhdr.
        if (outdir_bv_txt_u16 / bv_dir / f"{base3u}.txt").exists():
            raise AssertionError("expected extractor to rename UTF-16LE BrainVision .txt header to .vhdr")

        # Basic encoding sanity: expect many NUL bytes and no BOM.
        vhdr_bytes_u16 = exp_vhdr_u16.read_bytes()
        if vhdr_bytes_u16.startswith(b"\xff\xfe") or vhdr_bytes_u16.startswith(b"\xfe\xff"):
            raise AssertionError("expected UTF-16LE header to remain without BOM")
        if vhdr_bytes_u16.count(b"\x00") < 10:
            raise AssertionError("expected UTF-16LE header to contain NUL bytes")

        # Ensure we can decode the extracted header as UTF-16LE and it contains the expected references.
        vhdr_txt_u16 = vhdr_bytes_u16.decode("utf-16-le", errors="ignore")
        if f"DataFile={base3u}.eeg" not in vhdr_txt_u16 and f"DataFile = {base3u}.eeg" not in vhdr_txt_u16:
            print(vhdr_txt_u16)
            raise AssertionError("expected UTF-16LE .vhdr to contain DataFile reference")
        if f"MarkerFile={base3u}.vmrk" not in vhdr_txt_u16 and f"MarkerFile = {base3u}.vmrk" not in vhdr_txt_u16:
            print(vhdr_txt_u16)
            raise AssertionError("expected UTF-16LE .vhdr to contain MarkerFile reference")

        if exp_eeg_u16.read_bytes() != eeg_bytes3u:
            raise AssertionError("BrainVision UTF-16LE .txt header: .eeg bytes mismatch")


        # 2e) BrainVision header stored as .ini in a subdirectory, using ".." references.
        # Some exporters (and some containers) store the BrainVision header in a non-.vhdr file,
        # and the DataFile/MarkerFile entries may use ".." to reference sibling files.
        bv_container_ini = td_path / "session.brainvision_ini_rel.m2k"
        base4 = "bv_session_004"

        vhdr_text4 = (
            "Brain Vision Data Exchange Header File Version 1.0\n"
            "[Common Infos]\n"
            f"DataFile=../{base4}.eeg\n"
            f"MarkerFile=../{base4}.vmrk\n"
            "DataFormat=BINARY\n"
        )
        eeg_bytes4 = b"EEGDUMMY4" + b"\x09\x0A\x0B\x0C" * 16
        vmrk_text4 = (
            "Brain Vision Data Exchange Marker File Version 1.0\n"
            "[Common Infos]\n"
            f"DataFile={base4}.eeg\n"
            "[Marker Infos]\n"
            "Mk1=Stimulus,S 1,1,1,0\n"
        )

        with zipfile.ZipFile(str(bv_container_ini), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            # Store the header as .ini inside a subdir.
            zf.writestr(f"{bv_dir}/sub/{base4}.ini", vhdr_text4.encode("utf-8"))
            zf.writestr(f"{bv_dir}/{base4}.eeg", eeg_bytes4)
            zf.writestr(f"{bv_dir}/{base4}.vmrk", vmrk_text4.encode("utf-8"))

        # --list should include the .ini member.
        r = _run(["--input", str(bv_container_ini), "--list"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision .ini --list failed")
        if f"{base4}.ini" not in r.stdout:
            print(r.stdout)
            raise AssertionError("BrainVision .ini --list missing expected member")

        # Default extract should treat it as a BrainVision header and rename it to .vhdr,
        # extracting the referenced triplet and preserving ".." references.
        outdir_bv_ini = td_path / "out_bv_ini_rel"
        r = _run(["--container", str(bv_container_ini), "--outdir", str(outdir_bv_ini), "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("BrainVision .ini extract failed")

        exp_vhdr_ini = outdir_bv_ini / bv_dir / "sub" / f"{base4}.vhdr"
        exp_eeg_ini = outdir_bv_ini / bv_dir / f"{base4}.eeg"
        exp_vmrk_ini = outdir_bv_ini / bv_dir / f"{base4}.vmrk"

        for p in (exp_vhdr_ini, exp_eeg_ini, exp_vmrk_ini):
            if not p.exists():
                raise AssertionError(f"expected extracted BrainVision file missing (.ini relpath test): {p}")

        # Ensure the original .ini header name was converted to .vhdr.
        if (outdir_bv_ini / bv_dir / "sub" / f"{base4}.ini").exists():
            raise AssertionError("expected extractor to rename BrainVision .ini header to .vhdr")

        vhdr_out4 = exp_vhdr_ini.read_text(encoding="utf-8", errors="ignore")
        if f"DataFile=../{base4}.eeg" not in vhdr_out4 and f"DataFile = ../{base4}.eeg" not in vhdr_out4:
            print(vhdr_out4)
            raise AssertionError("expected extractor to preserve/patch DataFile .. reference in .vhdr")
        if f"MarkerFile=../{base4}.vmrk" not in vhdr_out4 and f"MarkerFile = ../{base4}.vmrk" not in vhdr_out4:
            print(vhdr_out4)
            raise AssertionError("expected extractor to preserve/patch MarkerFile .. reference in .vhdr")

        vmrk_out4 = exp_vmrk_ini.read_text(encoding="utf-8", errors="ignore")
        if f"DataFile={base4}.eeg" not in vmrk_out4 and f"DataFile = {base4}.eeg" not in vmrk_out4:
            print(vmrk_out4)
            raise AssertionError("expected extractor to patch vmrk DataFile to match extracted .eeg")

        # 3) ZipSlip protection: member tries to escape.
        bad_container = td_path / "bad.m2k"
        with zipfile.ZipFile(str(bad_container), "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("../evil.edf", payload_bytes)

        outdir2 = td_path / "out2"
        r = _run(["--input", str(bad_container), "--outdir", str(outdir2), "--print"], repo_root)
        # The script should fail (non-zero) because the only candidate is unsafe.
        if r.returncode == 0:
            print(r.stdout)
            raise AssertionError("expected ZipSlip protection failure")
        # Ensure it did not write outside outdir2.
        if (td_path / "evil.edf").exists():
            raise AssertionError("ZipSlip wrote outside expected outdir")

    print("selftest_biotrace_extract: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
