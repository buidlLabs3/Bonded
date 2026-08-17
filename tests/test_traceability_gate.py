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

    def readiness(self, directory: str, **updates) -> Path:
        criterion = {
            "id": "REQ-01",
            "requirement": "Requirement",
            "owner": "release",
            "required_scope": "clean-clone",
            "status": "open",
            "verification_command": "manual verification",
            "pass_condition": "The verification command passes.",
            "evidence": [],
            "gap": "Not verified yet.",
        }
        criterion.update(updates)
        path = Path(directory) / "readiness.json"
        path.write_text(
            __import__("json").dumps(
                {
                    "schema_version": 1,
                    "specification": "https://example.test/spec",
                    "release_commit": "abc",
                    "overall_status": "not-ready",
                    "criteria": [criterion],
                }
            ),
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

    def test_readiness_requires_complete_fields_and_existing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            document = traceability.readiness(self.readiness(directory), Path(directory))
            self.assertEqual(document["criteria"][0]["id"], "REQ-01")
            with self.assertRaisesRegex(traceability.TraceabilityGateError, "missing evidence"):
                traceability.readiness(
                    self.readiness(directory, evidence=["missing.json"]), Path(directory)
                )

    def test_readiness_rejects_a_missing_verification_command(self):
        with tempfile.TemporaryDirectory() as directory:
            audit = self.readiness(
                directory, verification_command="scripts/does-not-exist.sh"
            )
            with self.assertRaisesRegex(traceability.TraceabilityGateError, "missing command"):
                traceability.readiness(audit, Path(directory))

    def test_criterion_gate_fails_closed_until_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            open_audit = self.readiness(directory)
            with self.assertRaisesRegex(traceability.TraceabilityGateError, "is open"):
                traceability.verify_criterion(
                    open_audit, Path("evidence.json"), Path(directory), "REQ-01"
                )

            evidence = Path(directory) / "evidence.json"
            evidence.write_text("{}", encoding="utf-8")
            verified_audit = self.readiness(
                directory, status="verified-local", evidence=["evidence.json"]
            )
            report = traceability.verify_criterion(
                verified_audit, Path("index.json"), Path(directory), "REQ-01"
            )
            self.assertEqual(report["criterion_status"], "verified-local")

    def test_verified_readiness_claim_triggers_testnet_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "evidence.json"
            evidence.write_text("{}", encoding="utf-8")
            audit = self.readiness(
                directory,
                status="verified-testnet",
                required_scope="public-testnet",
                evidence=["evidence.json"],
            )
            fake_gate = mock.Mock()
            fake_gate.audit.return_value = {"status": "pass", "failed_count": 0}
            with mock.patch.object(traceability, "_load_evidence_gate", return_value=fake_gate):
                report = traceability.audit(
                    self.matrix(directory, "implemented"),
                    Path("index.json"),
                    Path(directory),
                    audit,
                )
            self.assertEqual(report["readiness_verified_testnet_claims"], ["REQ-01"])
            self.assertEqual(report["evidence_gate"], "pass")


if __name__ == "__main__":
    unittest.main()
