#!/usr/bin/env python3

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "traceability_gate", REPO / "tools" / "traceability_gate.py"
)
traceability = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(traceability)


class TraceabilityGateTests(unittest.TestCase):
    def matrix(self, directory: str, status: str, duplicate: bool = False) -> Path:
        path = Path(directory) / "traceability.md"
        row = f"| REQ-01 | Requirement | 20 | evidence | {status} |\n"
        path.write_text(
            "| ID | Requirement | Chunk | Verification | Status |\n"
            "|---|---|---:|---|---|\n"
            + row
            + (row if duplicate else ""),
            encoding="utf-8",
        )
        return path

    def test_incomplete_matrix_needs_no_testnet_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            report = traceability.audit(
                self.matrix(directory, "implemented"), Path("missing.json"), Path(directory)
            )
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["evidence_gate"], "not-required")

    def test_verified_testnet_claim_requires_complete_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            matrix = self.matrix(directory, "verified-testnet")
            failed = {"status": "fail", "failed_count": 1}
            fake_gate = mock.Mock()
            fake_gate.audit.return_value = failed
            with mock.patch.object(traceability, "_load_evidence_gate", return_value=fake_gate):
                with self.assertRaisesRegex(
                    traceability.TraceabilityGateError, "complete offline"
                ):
                    traceability.audit(matrix, Path("index.json"), Path(directory))

            fake_gate.audit.return_value = {"status": "pass", "failed_count": 0}
            with mock.patch.object(traceability, "_load_evidence_gate", return_value=fake_gate):
                report = traceability.audit(matrix, Path("index.json"), Path(directory))
            self.assertEqual(report["verified_testnet_claims"], ["REQ-01"])
            self.assertEqual(report["evidence_gate"], "pass")

    def test_unknown_status_and_duplicate_rows_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(traceability.TraceabilityGateError, "unsupported"):
                traceability.rows(self.matrix(directory, "done"))
            with self.assertRaisesRegex(traceability.TraceabilityGateError, "duplicate"):
                traceability.rows(self.matrix(directory, "planned", duplicate=True))


if __name__ == "__main__":
    unittest.main()
