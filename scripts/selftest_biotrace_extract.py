#!/usr/bin/env python3
"""selftest_biotrace_extract.py

Smoke-test for scripts/biotrace_extract_container.py.

This test:
  - builds a tiny ZIP file with a dummy .edf payload (using the .bcd extension)
  - runs the extractor in both --list and extract modes
  - verifies extraction output
  - verifies ZipSlip protection (refuses unsafe paths)

No third-party dependencies.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def _run(args, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "scripts/biotrace_extract_container.py"] + args,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory(prefix="qeeg_biotrace_extract_") as td:
        td_path = Path(td)

        # Build a dummy .bcd (ZIP) with a plausible recording path inside.
        container_path = td_path / "session.bcd"
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

        # Extract the best payload.
        outdir = td_path / "out"
        r = _run(["--input", str(container_path), "--outdir", str(outdir), "--print"], repo_root)
        if r.returncode != 0:
            print(r.stdout)
            raise AssertionError("extract failed")
        extracted_path = Path(r.stdout.strip().splitlines()[-1])
        if not extracted_path.exists():
            raise AssertionError(f"extracted file missing: {extracted_path}")
        if extracted_path.read_bytes() != payload_bytes:
            raise AssertionError("extracted bytes mismatch")

        # ZipSlip protection: member tries to escape.
        bad_container = td_path / "bad.bcd"
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
