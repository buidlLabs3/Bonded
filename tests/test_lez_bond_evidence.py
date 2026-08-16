#!/usr/bin/env python3

import importlib.util
import json
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_bond_evidence", REPO / "tools" / "lez_bond_evidence.py"
)
evidence = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evidence)


class BondEvidenceTests(unittest.TestCase):
    def test_default_deployment_is_the_corrected_canonical_release(self):
        self.assertEqual(
            evidence.DEFAULT_DEPLOYMENT,
            REPO / "evidence/testnet/settlement-program.json",
        )
        deployment = json.loads(evidence.DEFAULT_DEPLOYMENT.read_text(encoding="utf-8"))
        self.assertEqual(deployment["program_id"], evidence.lez_bond.CANONICAL_PROGRAM_ID)

    def fixture(self, root: Path, inventory_operation="acceptance-initialize"):
        operation, outcome = evidence.OPERATIONS[inventory_operation]
        accounts = {
            "sender": "11111111111111111111111111111111",
            "owner": "4vJ9JU1bJJE96FWSJKvHsmmFZJdWKPa2HUNfLNy8FhUB",
            "sink": "8qbHbw2BbbTHBW1sbeqakYXVWCLmLqJH4LrkYyRNCCpv",
            "state": "CktRuQ2mttgRGkXJ8ZrUaHA4SKBBGWmuSQ9C9EGPBLNk",
            "escrow": "GgBaCs3N4sQtPAi6M9vZzL4wYFZc7W8Q6e6v8oG8q1jT",
        }
        if operation == "settle":
            accounts["destination"] = (
                accounts["sink"] if outcome == "sink-rejected" else accounts["sender"]
            )
        network_identity = {
            "channel_id": "01" * 32,
            "lez_release": "v0.2.4",
            "lez_release_commit": "47eba256479f6f785acbd138834340703cd03401",
        }
        deployment = {
            "network_identity": network_identity,
            "program_id": evidence.lez_bond.CANONICAL_PROGRAM_ID,
            "binary_sha256": "12" * 32,
            "binary_size": 368324,
        }
        candidate = {
            **deployment,
            "schema_version": 1,
            "status": "official-wallet-sequencer-finalized-candidate",
            "network": "lez-testnet",
            "operation": operation,
            "outcome": outcome,
            "bond_id": "34" * 32,
            "accounts": accounts,
            "instruction_word_count": 199 if operation == "initialize" else 2,
            "instruction_words_sha256": "56" * 32,
            "transaction": "ab" * 32,
            "transaction_type": "PrivacyPreserving",
            "block": 42,
            "block_hash": "cd" * 32,
            "finality": "Finalized",
            "state": {"before": {}, "after": {}},
        }
        candidate_path = root / "candidate.json"
        deployment_path = root / "deployment.json"
        candidate_path.write_text(json.dumps(candidate), encoding="utf-8")
        deployment_path.write_text(json.dumps(deployment), encoding="utf-8")
        args = types.SimpleNamespace(
            candidate=candidate_path,
            deployment=deployment_path,
            operation=inventory_operation,
            verifier_commit="78" * 20,
            observer="primary",
            confirmations=3,
            evidence=root / "observation.json",
        )
        return args, candidate

    def reconciled(self):
        return {
            "status": "finalized",
            "transaction": "ab" * 32,
            "transaction_type": "PrivacyPreserving",
            "block": 42,
            "block_hash": "cd" * 32,
            "checks": {"all": True},
        }

    def test_initialize_promotion_binds_exact_three_public_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            args, candidate = self.fixture(Path(directory))
            with mock.patch.object(
                evidence.lez_explorer,
                "reconcile_transaction",
                return_value=self.reconciled(),
            ) as reconcile:
                result = evidence.promote(args)
            called = reconcile.call_args.args[0]
            self.assertEqual(called.transaction_type, "PrivacyPreserving")
            self.assertEqual(called.program_id, None)
            self.assertEqual(
                called.account_id,
                [candidate["accounts"][name] for name in ("sender", "state", "escrow")],
            )
            self.assertEqual(result["bond_provenance"]["program_id"], candidate["program_id"])

    def test_rejection_settlement_requires_sink_destination_and_four_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            args, candidate = self.fixture(Path(directory), "rejection-settle")
            with mock.patch.object(
                evidence.lez_explorer,
                "reconcile_transaction",
                return_value=self.reconciled(),
            ) as reconcile:
                evidence.promote(args)
            self.assertEqual(
                reconcile.call_args.args[0].account_id,
                [
                    candidate["accounts"][name]
                    for name in ("state", "escrow", "destination", "owner")
                ],
            )
            candidate["accounts"]["destination"] = candidate["accounts"]["sender"]
            args.candidate.write_text(json.dumps(candidate), encoding="utf-8")
            args.evidence = Path(directory) / "invalid.json"
            with self.assertRaisesRegex(evidence.BondEvidenceError, "destination"):
                evidence.promote(args)

    def test_expiry_settlement_omits_mutable_owner_and_clock_accounts(self):
        with tempfile.TemporaryDirectory() as directory:
            args, candidate = self.fixture(Path(directory), "expiry-settle")
            with mock.patch.object(
                evidence.lez_explorer,
                "reconcile_transaction",
                return_value=self.reconciled(),
            ) as reconcile:
                evidence.promote(args)
            self.assertEqual(
                reconcile.call_args.args[0].account_id,
                [candidate["accounts"][name] for name in ("state", "escrow", "destination")],
            )

    def test_nonfinal_or_noncanonical_candidate_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args, candidate = self.fixture(Path(directory))
            candidate["status"] = "submitted"
            args.candidate.write_text(json.dumps(candidate), encoding="utf-8")
            with self.assertRaisesRegex(evidence.BondEvidenceError, "not a finalized"):
                evidence.promote(args)
            candidate["status"] = "official-wallet-sequencer-finalized-candidate"
            candidate["binary_sha256"] = "ff" * 32
            args.candidate.write_text(json.dumps(candidate), encoding="utf-8")
            with self.assertRaisesRegex(evidence.BondEvidenceError, "binary_sha256"):
                evidence.promote(args)

    def test_explorer_conflict_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            args, _candidate = self.fixture(Path(directory))
            result = self.reconciled()
            result["block_hash"] = "ef" * 32
            with mock.patch.object(
                evidence.lez_explorer, "reconcile_transaction", return_value=result
            ):
                with self.assertRaisesRegex(evidence.BondEvidenceError, "conflicts"):
                    evidence.promote(args)
            self.assertFalse(args.evidence.exists())

    def test_output_uses_immutable_writer(self):
        with tempfile.TemporaryDirectory() as directory:
            args, _candidate = self.fixture(Path(directory))
            with mock.patch.object(
                evidence.lez_explorer,
                "reconcile_transaction",
                return_value=self.reconciled(),
            ):
                evidence.promote(args)
                with self.assertRaises(FileExistsError):
                    evidence.promote(args)


if __name__ == "__main__":
    unittest.main()
