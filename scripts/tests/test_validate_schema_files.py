import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "scripts" / "validate_schema_files.py"

_DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"


def _write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write("\n")


def _base_schema(filename: str, *, schema_id_base: str = "https://example.com/") -> dict:
    # validate_schema_files.py enforces that $id ends with the filename.
    return {
        "$schema": _DRAFT_2020_12,
        "$id": f"{schema_id_base.rstrip('/')}/{filename}",
        "type": "object",
        "additionalProperties": False,
        "properties": {},
    }


def _run_validator(schemas_dir: Path, *extra_args: str):
    cmd = [
        sys.executable,
        "-B",
        str(_SCRIPT),
        "--schemas-dir",
        str(schemas_dir),
    ]
    cmd.extend(extra_args)

    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    proc = subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        cwd=str(_REPO_ROOT),
    )
    return proc.returncode, proc.stdout, proc.stderr


class TestValidateSchemaFiles(unittest.TestCase):
    def test_ok_minimal_schemas(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            schemas = Path(td)

            b = _base_schema("b.schema.json")
            b["$defs"] = {
                "thing": {
                    "type": "string",
                }
            }
            _write_json(schemas / "b.schema.json", b)

            a = _base_schema("a.schema.json")
            a["properties"] = {
                "x": {
                    "$ref": "b.schema.json#/$defs/thing",
                }
            }
            _write_json(schemas / "a.schema.json", a)

            code, out, err = _run_validator(schemas)
            self.assertEqual(code, 0, msg=f"stdout=\n{out}\nstderr=\n{err}")
            self.assertIn("OK: validated", out)

    def test_duplicate_id_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            schemas = Path(td)

            s1 = _base_schema("one.schema.json", schema_id_base="https://example.com/dup")
            s2 = _base_schema("two.schema.json", schema_id_base="https://example.com/dup")

            # Force duplicate $id while still satisfying the 'endswith filename' rule.
            s2["$id"] = s1["$id"]

            _write_json(schemas / "one.schema.json", s1)
            _write_json(schemas / "two.schema.json", s2)

            code, out, err = _run_validator(schemas, "--no-ref-check")
            self.assertNotEqual(code, 0)
            self.assertIn("duplicate $id", err)

    def test_bad_ref_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            schemas = Path(td)

            a = _base_schema("a.schema.json")
            a["properties"] = {
                "x": {
                    "$ref": "#/$defs/missing",
                }
            }
            _write_json(schemas / "a.schema.json", a)

            code, out, err = _run_validator(schemas)
            self.assertNotEqual(code, 0)
            self.assertIn("$ref error", err)

    def test_require_jsonschema_flag(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            schemas = Path(td)

            a = _base_schema("a.schema.json")
            _write_json(schemas / "a.schema.json", a)

            code, out, err = _run_validator(schemas, "--require-jsonschema")
            self.assertEqual(code, 0, msg=f"stdout=\n{out}\nstderr=\n{err}")
            self.assertIn("jsonschema", out)


if __name__ == "__main__":
    unittest.main()
