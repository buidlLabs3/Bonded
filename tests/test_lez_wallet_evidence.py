#!/usr/bin/env python3

import importlib.util
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_wallet_evidence", REPO / "tools" / "lez_wallet_evidence.py"
)
evidence = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evidence)


class WalletEvidenceTests(unittest.TestCase):
    def args(self, operation="register:sender"):
        return types.SimpleNamespace(
            candidate=Path("candidate.json"),
            operation=operation,
            verifier_commit="12" * 20,
            observer="primary",
            confirmations=3,
            evidence=Path("observation.json"),
        )

    def candidate(self, operation="register:sender"):
        return {
            "accounts": {
                "sender": "6oFNU77YaPqJVB69t1rPwGdYeULXixugNR9KJiNocWLR",
                "owner": "EsxbHEvBVTKAcz2vPq46NSi1AG9pbVYk65LyNfRQknZV",
                "sink": "FW6NaTcfeb354webZg3UqDLMVUFKv166mGTX6bxGswua",
            },
            "operations": [
                {
                    "id": operation,
                    "status": "finalized",
                    "transaction": "ab" * 32,
                    "transaction_type": "Public",
                    "block": 42,
                    "block_hash": "cd" * 32,
                    "finality": "Finalized",
                }
            ],
        }

    def test_submitted_candidate_cannot_be_promoted(self):
        candidate = self.candidate()
        candidate["operations"][0]["status"] = "submitted"
        with mock.patch.object(evidence.candidate_status, "load_candidate", return_value=candidate):
            with self.assertRaisesRegex(evidence.WalletEvidenceError, "not finalized"):
                evidence.promote(self.args())

    def test_registration_promotion_binds_exact_public_identifiers(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args()
            args.evidence = Path(directory) / "observation.json"
            candidate = self.candidate()
            result = {
                "status": "finalized",
                "transaction": "ab" * 32,
                "block_hash": "cd" * 32,
                "checks": {"all": True},
            }
            with mock.patch.object(
                evidence.candidate_status, "load_candidate", return_value=candidate
            ):
                with mock.patch.object(
                    evidence.lez_explorer, "reconcile_transaction", return_value=result
                ) as reconcile:
                    self.assertEqual(evidence.promote(args), result)
        args = reconcile.call_args.args[0]
        self.assertEqual(args.transaction_type, "Public")
        self.assertEqual(args.kind, "wallet-registration")
        self.assertEqual(args.operation, "register-sender")
        self.assertEqual(args.account_id, [candidate["accounts"]["sender"]])
        self.assertEqual(
            args.program_id, evidence.candidate_status.AUTHENTICATED_TRANSFER_PROGRAM_ID
        )

    def test_funding_promotion_binds_pinata_and_sender(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args("fund:sender")
            args.evidence = Path(directory) / "observation.json"
            candidate = self.candidate("fund:sender")
            result = {
                "status": "finalized",
                "transaction": "ab" * 32,
                "block_hash": "cd" * 32,
                "checks": {"all": True},
            }
            with mock.patch.object(
                evidence.candidate_status, "load_candidate", return_value=candidate
            ):
                with mock.patch.object(
                    evidence.lez_explorer, "reconcile_transaction", return_value=result
                ) as reconcile:
                    evidence.promote(args)
        args = reconcile.call_args.args[0]
        self.assertEqual(args.kind, "wallet-funding")
        self.assertEqual(
            args.account_id,
            [evidence.candidate_status.PINATA_ACCOUNT, candidate["accounts"]["sender"]],
        )
        self.assertEqual(args.program_id, evidence.candidate_status.PINATA_PROGRAM_ID)

    def test_conflicting_explorer_result_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args()
            args.evidence = Path(directory) / "observation.json"
            candidate = self.candidate()
            result = {
                "status": "finalized",
                "transaction": "ab" * 32,
                "block_hash": "ef" * 32,
                "checks": {"all": True},
            }
            with mock.patch.object(
                evidence.candidate_status, "load_candidate", return_value=candidate
            ):
                with mock.patch.object(
                    evidence.lez_explorer, "reconcile_transaction", return_value=result
                ):
                    with self.assertRaisesRegex(evidence.WalletEvidenceError, "conflicts"):
                        evidence.promote(args)
            self.assertFalse(args.evidence.exists())

    def test_output_is_created_by_the_immutable_explorer_writer(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args()
            args.evidence = Path(directory) / "new" / "observation.json"
            candidate = self.candidate()

            result = {
                "status": "finalized",
                "transaction": "ab" * 32,
                "block_hash": "cd" * 32,
                "checks": {"all": True},
            }

            with mock.patch.object(
                evidence.candidate_status, "load_candidate", return_value=candidate
            ):
                with mock.patch.object(
                    evidence.lez_explorer, "reconcile_transaction", return_value=result
                ):
                    evidence.promote(args)
                    with self.assertRaises(FileExistsError):
                        evidence.promote(args)


if __name__ == "__main__":
    unittest.main()
