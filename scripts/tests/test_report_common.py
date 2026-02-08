#!/usr/bin/env python3

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path


# Ensure scripts/ is on sys.path so we can import report_common.py
_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))

import report_common as rc  # noqa: E402


class ReportCommonTests(unittest.TestCase):
    def setUp(self) -> None:
        os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
        sys.dont_write_bytecode = True

    def test_posix_relpath_basic_has_forward_slashes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="qeeg_report_common_test_") as td:
            base = Path(td)
            (base / "a" / "b").mkdir(parents=True)
            target = base / "a" / "b" / "file.txt"
            target.write_text("x", encoding="utf-8")

            rel = rc.posix_relpath(str(target), str(base))

            self.assertIn("/", rel)
            self.assertNotIn("\\", rel)

    @unittest.skipUnless(os.name == "nt", "Windows-only cross-drive behavior")
    def test_posix_relpath_cross_drive_never_raises(self) -> None:
        # os.path.relpath raises ValueError for different drive letters on Windows.
        rel = rc.posix_relpath("D:\\out\\file.html", "C:\\base")
        self.assertNotIn("\\", rel)
        self.assertTrue(rel.lower().startswith("d:/"))


if __name__ == "__main__":
    raise SystemExit(unittest.main())
