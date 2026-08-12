#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_evidence_gate", REPO / "tools" / "lez_evidence_gate.py"
)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class EvidenceGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "evidence/testnet").mkdir(parents=True)

    def tearDown(self):
        self.temporary.cleanup()

    def _write_json(self, relative: str, value: dict) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        return path

    def _entry(self, identifier="transfer-one", transaction_type="Public"):
        return {
            "id": identifier,
            "kind": "test-operation",
            "component": "test-component",
            "operation": identifier,
            "transaction_type": transaction_type,
            "observations": [
                f"evidence/testnet/{identifier}/first.json",
                f"evidence/testnet/{identifier}/second.json",
            ],
            "screenshots": {
                "transaction": f"evidence/testnet/{identifier}/transaction.json",
                "block": f"evidence/testnet/{identifier}/block.json",
            },
        }

    def _observation(self, entry, tip, tx="ab" * 32, observer="primary"):
        block = 10
        block_hash = "cd" * 32
        return {
            "schema_version": 1,
            "status": "finalized",
            "kind": entry["kind"],
            "component": entry["component"],
            "operation": entry["operation"],
            "network": "lez-testnet",
            "network_identity": {
                "channel_id": "01" * 32,
                "lez_release": "v0.2.4",
                "lez_release_commit": "47eba256479f6f785acbd138834340703cd03401",
            },
            "services": {
                "sequencer": "https://testnet.lez.logos.co",
                "explorer": "https://explorer.testnet.lez.logos.co",
            },
            "transaction": tx,
            "transaction_type": entry["transaction_type"],
            "block": block,
            "block_hash": block_hash,
            "finality": "Finalized",
            "observed_at_utc": f"2026-08-12T00:00:{tip:02d}+00:00",
            "confirmation_depth": tip - block,
            "required_confirmation_depth": 3,
            "explorer_urls": {
                "transaction": f"https://explorer.testnet.lez.logos.co/transaction/{tx}",
                "block": f"https://explorer.testnet.lez.logos.co/block/{block}",
            },
            "checks": {"all_identifiers_match": True},
            "verifier": {
                "source": "tools/lez_explorer.py",
                "source_commit": "12" * 20,
                "observer": observer,
                "result": "pass",
            },
            "observations": {
                "explorer_latest_finalized": {"block_id": tip},
                "overlapping_finalized_blocks": [{"matches": True}] * 3,
            },
        }

    def _sidecar(self, entry, page, observation):
        directory = self.root / "evidence/testnet" / entry["id"]
        directory.mkdir(parents=True, exist_ok=True)
        png = b"\x89PNG\r\n\x1a\nfixture"
        png_path = directory / f"{page}.png"
        png_path.write_bytes(png)
        if page == "transaction":
            expected = [
                observation["transaction"],
                gate.TRANSACTION_TITLES[observation["transaction_type"]],
            ]
        else:
            expected = [
                observation["transaction"],
                str(observation["block"]),
                observation["block_hash"],
                "Finalized",
            ]
        sidecar = {
            "schema_version": 1,
            "url": observation["explorer_urls"][page],
            "expected_rendered_text": expected,
            "rendered_text_sha256": hashlib.sha256("\n".join(expected).encode()).hexdigest(),
            "screenshot": png_path.name,
            "screenshot_sha256": hashlib.sha256(png).hexdigest(),
            "screenshot_bytes": len(png),
            "dimensions": {"width": 800, "height": 600},
            "browser": "Fixture Browser",
            "fresh_profile": True,
            "navigation_error": None,
            "observed_at_utc": "2026-08-12T00:01:00+00:00",
        }
        return self._write_json(entry["screenshots"][page], sidecar)

    def _bundle(self, entries):
        for entry in entries:
            first = self._observation(entry, 13)
            second = self._observation(entry, 16, observer="independent")
            self._write_json(entry["observations"][0], first)
            self._write_json(entry["observations"][1], second)
            self._sidecar(entry, "transaction", second)
            self._sidecar(entry, "block", second)
        index = {"schema_version": 1, "network": "lez-testnet", "required_transactions": entries}
        return self._write_json("evidence/testnet/required-evidence.json", index)

    def test_complete_two_observation_bundle_passes_offline(self):
        index = self._bundle([self._entry()])
        report = gate.audit(index, self.root)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["passed_count"], 1)
        self.assertEqual(len(report["results"][0]["artifact_sha256"]), 5)

    def test_missing_artifact_and_wrong_type_fail_named_row(self):
        entry = self._entry()
        index = self._bundle([entry])
        (self.root / entry["observations"][1]).unlink()
        report = gate.audit(index, self.root)
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["failures"][0]["id"], entry["id"])
        self.assertIn("missing", report["failures"][0]["error"])

        second = self._observation(entry, 16, observer="independent")
        second["transaction_type"] = "PrivacyPreserving"
        self._write_json(entry["observations"][1], second)
        report = gate.audit(index, self.root)
        self.assertIn("type", report["failures"][0]["error"])

    def test_secret_fields_and_insufficient_separation_fail(self):
        entry = self._entry()
        index = self._bundle([entry])
        second_path = self.root / entry["observations"][1]
        second = json.loads(second_path.read_text(encoding="utf-8"))
        second["private_key"] = "not-allowed"
        self._write_json(entry["observations"][1], second)
        report = gate.audit(index, self.root)
        self.assertIn("sensitive", report["failures"][0]["error"])

        second.pop("private_key")
        second["observations"]["explorer_latest_finalized"]["block_id"] = 15
        self._write_json(entry["observations"][1], second)
        report = gate.audit(index, self.root)
        self.assertIn("separated", report["failures"][0]["error"])

    def test_stale_latest_observation_fails(self):
        entry = self._entry()
        index = self._bundle([entry])
        path = self.root / entry["observations"][1]
        second = json.loads(path.read_text(encoding="utf-8"))
        second["observed_at_utc"] = (
            datetime.now(timezone.utc) - timedelta(days=8)
        ).replace(microsecond=0).isoformat()
        first_path = self.root / entry["observations"][0]
        first = json.loads(first_path.read_text(encoding="utf-8"))
        first["observed_at_utc"] = (
            datetime.now(timezone.utc) - timedelta(days=9)
        ).replace(microsecond=0).isoformat()
        self._write_json(entry["observations"][0], first)
        self._write_json(entry["observations"][1], second)
        report = gate.audit(index, self.root)
        self.assertIn("stale", report["failures"][0]["error"])

    def test_tampered_screenshot_and_hash_reuse_fail(self):
        first = self._entry("transfer-one")
        second = self._entry("transfer-two")
        index = self._bundle([first, second])
        report = gate.audit(index, self.root)
        self.assertEqual(report["status"], "fail")
        self.assertIn("reused", report["failures"][0]["error"])

        index = self._bundle([first])
        screenshot = self.root / "evidence/testnet/transfer-one/transaction.png"
        screenshot.write_bytes(b"\x89PNG\r\n\x1a\ntampered")
        report = gate.audit(index, self.root)
        self.assertIn("byte count", report["failures"][0]["error"])

    def test_duplicate_json_key_is_rejected(self):
        path = self.root / "evidence/testnet/duplicate.json"
        path.write_text('{"status":"finalized","status":"failed"}', encoding="utf-8")
        with self.assertRaisesRegex(gate.EvidenceGateError, "duplicate JSON key"):
            gate.load_json(path)

    def test_live_service_failure_is_verification_unavailable(self):
        class FakeExplorer:
            class ExplorerValidationError(RuntimeError):
                pass

            class ExplorerTransportError(ExplorerValidationError):
                pass

            @staticmethod
            def reconcile_transaction(_args):
                raise FakeExplorer.ExplorerTransportError("official explorer unavailable")

        entry = self._entry()
        document = self._observation(entry, 16, observer="independent")
        latest = gate.validate_observation(document, entry)
        with mock.patch.object(gate, "_load_explorer", return_value=FakeExplorer):
            with self.assertRaisesRegex(gate.VerificationUnavailable, "unavailable"):
                gate.live_reconcile(entry, latest)

    def test_live_semantic_mismatch_is_a_hard_failure(self):
        class FakeExplorer:
            class ExplorerValidationError(RuntimeError):
                pass

            class ExplorerTransportError(ExplorerValidationError):
                pass

            @staticmethod
            def reconcile_transaction(_args):
                raise FakeExplorer.ExplorerValidationError("transaction not found")

        entry = self._entry()
        document = self._observation(entry, 16, observer="independent")
        latest = gate.validate_observation(document, entry)
        with mock.patch.object(gate, "_load_explorer", return_value=FakeExplorer):
            with self.assertRaisesRegex(FakeExplorer.ExplorerValidationError, "not found"):
                gate.live_reconcile(entry, latest)


if __name__ == "__main__":
    unittest.main()
