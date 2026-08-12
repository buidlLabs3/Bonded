#!/usr/bin/env python3

import base64
import hashlib
import importlib.util
import json
import struct
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_candidate_status", REPO / "tools" / "lez_candidate_status.py"
)
status = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(status)


def block(block_id=12, finality=2):
    raw = (
        struct.pack("<Q", block_id)
        + bytes.fromhex("11" * 32)
        + bytes.fromhex("22" * 32)
        + struct.pack("<Q", 123456)
        + bytes(64)
        + struct.pack("<I", 0)
        + bytes([finality])
    )
    return base64.b64encode(raw).decode()


class CandidateStatusTests(unittest.TestCase):
    def candidate(self, tx_hash="ab" * 32):
        return {
            "schema_version": 1,
            "network": "lez-testnet",
            "release_commit": status.lez_explorer.LEZ_RELEASE_COMMIT,
            "accounts": {
                "sender": "6oFNU77YaPqJVB69t1rPwGdYeULXixugNR9KJiNocWLR",
                "owner": "EsxbHEvBVTKAcz2vPq46NSi1AG9pbVYk65LyNfRQknZV",
                "sink": "FW6NaTcfeb354webZg3UqDLMVUFKv166mGTX6bxGswua",
            },
            "operations": [
                {
                    "id": "register:sender",
                    "status": "submitted",
                    "transaction": tx_hash,
                }
            ],
        }

    def test_service_failure_is_verification_unavailable(self):
        with mock.patch.object(
            status.lez_explorer,
            "rpc_call",
            side_effect=status.lez_explorer.ExplorerValidationError("network timeout"),
        ):
            report = status.audit(self.candidate(), "register:sender", 1, 3)
        self.assertEqual(report["status"], "verification-unavailable")
        self.assertNotIn("wallet", report)

    def test_absent_hash_is_not_sequencer_included(self):
        with mock.patch.object(status.lez_explorer, "rpc_call", return_value=None):
            report = status.audit(self.candidate(), "register:sender", 1, 3)
        self.assertEqual(report["status"], "not-sequencer-included")

    def test_nonfinal_block_stays_sequencer_included(self):
        serialized = b"public-transaction"
        tx_hash = hashlib.sha256(serialized).hexdigest()
        responses = [[base64.b64encode(b"\x00" + serialized).decode(), 12], block(12, 1)]
        with mock.patch.object(status.lez_explorer, "rpc_call", side_effect=responses):
            report = status.audit(self.candidate(tx_hash), "register:sender", 1, 3)
        self.assertEqual(report["status"], "sequencer-included")
        self.assertEqual(report["finality"], "Safe")

    def test_malformed_sequencer_bytes_are_disputed(self):
        responses = [[base64.b64encode(b"\xffinvalid").decode(), 12], block()]
        with mock.patch.object(status.lez_explorer, "rpc_call", side_effect=responses):
            report = status.audit(self.candidate(), "register:sender", 1, 3)
        self.assertEqual(report["status"], "disputed")

    def test_empty_explorer_tip_is_verification_unavailable(self):
        serialized = b"public-transaction"
        tx_hash = hashlib.sha256(serialized).hexdigest()
        rpc = [[base64.b64encode(b"\x00" + serialized).decode(), 12], block()]
        with mock.patch.object(status.lez_explorer, "rpc_call", side_effect=rpc):
            with mock.patch.object(status.lez_explorer, "fetch_explorer_page", return_value="root"):
                with mock.patch.object(status.lez_explorer, "explorer_recent_blocks", return_value=[]):
                    report = status.audit(self.candidate(tx_hash), "register:sender", 1, 3)
        self.assertEqual(report["status"], "verification-unavailable")

    def test_explorer_absence_is_lag_before_tip_and_dispute_after_tip(self):
        serialized = b"public-transaction"
        tx_hash = hashlib.sha256(serialized).hexdigest()
        rpc = [[base64.b64encode(b"\x00" + serialized).decode(), 12], block()]
        transaction_error = status.lez_explorer.ExplorerValidationError(
            "official explorer rejected transaction: Transaction not found"
        )
        with mock.patch.object(status.lez_explorer, "rpc_call", side_effect=rpc):
            with mock.patch.object(
                status.lez_explorer,
                "fetch_explorer_page",
                side_effect=["root", "transaction"],
            ):
                with mock.patch.object(
                    status.lez_explorer,
                    "explorer_recent_blocks",
                    return_value=[{"header": {"block_id": 11}}],
                ):
                    with mock.patch.object(
                        status.lez_explorer,
                        "explorer_transaction",
                        side_effect=transaction_error,
                    ):
                        report = status.audit(self.candidate(tx_hash), "register:sender", 1, 3)
        self.assertEqual(report["status"], "pending-indexer")

        rpc = [[base64.b64encode(b"\x00" + serialized).decode(), 12], block()]
        with mock.patch.object(status.lez_explorer, "rpc_call", side_effect=rpc):
            with mock.patch.object(
                status.lez_explorer,
                "fetch_explorer_page",
                side_effect=["root", "transaction"],
            ):
                with mock.patch.object(
                    status.lez_explorer,
                    "explorer_recent_blocks",
                    return_value=[{"header": {"block_id": 13}}],
                ):
                    with mock.patch.object(
                        status.lez_explorer,
                        "explorer_transaction",
                        side_effect=transaction_error,
                    ):
                        report = status.audit(self.candidate(tx_hash), "register:sender", 1, 3)
        self.assertEqual(report["status"], "disputed")

    def test_finalized_reconciliation_binds_program_and_account(self):
        serialized = b"public-transaction"
        tx_hash = hashlib.sha256(serialized).hexdigest()
        account = self.candidate(tx_hash)["accounts"]["sender"]
        indexed_payload = {
            "hash": tx_hash,
            "message": {
                "program_id": status.lez_explorer.explorer_program_id(
                    status.AUTHENTICATED_TRANSFER_PROGRAM_ID
                ),
                "account_ids": [account],
            },
        }
        reconciled = {
            "status": "finalized",
            "checks": {"all": True},
            "explorer_urls": {"transaction": "tx", "block": "block"},
        }
        rpc = [[base64.b64encode(b"\x00" + serialized).decode(), 12], block()]
        with mock.patch.object(status.lez_explorer, "rpc_call", side_effect=rpc):
            with mock.patch.object(
                status.lez_explorer,
                "fetch_explorer_page",
                side_effect=["root", "transaction"],
            ):
                with mock.patch.object(
                    status.lez_explorer,
                    "explorer_recent_blocks",
                    return_value=[{"header": {"block_id": 16}}],
                ):
                    with mock.patch.object(
                        status.lez_explorer,
                        "explorer_transaction",
                        return_value={"Public": indexed_payload},
                    ):
                        with mock.patch.object(
                            status.lez_explorer,
                            "reconcile_transaction",
                            return_value=reconciled,
                        ) as reconcile:
                            report = status.audit(
                                self.candidate(tx_hash), "register:sender", 1, 3
                            )
        self.assertEqual(report["status"], "finalized")
        args = reconcile.call_args.args[0]
        self.assertEqual(args.program_id, status.AUTHENTICATED_TRANSFER_PROGRAM_ID)
        self.assertEqual(args.account_id, [account])
        self.assertEqual(args.operation, "register-sender")

    def test_operation_must_be_unambiguously_submitted(self):
        candidate = self.candidate()
        candidate["operations"][0]["status"] = "submitting"
        with self.assertRaisesRegex(status.CandidateStatusError, "unambiguous"):
            status.select_operation(candidate, "register:sender")

    def test_candidate_accounts_must_be_distinct_canonical_base58(self):
        candidate = self.candidate()
        candidate["accounts"]["owner"] = candidate["accounts"]["sender"]
        with mock.patch.object(Path, "resolve", return_value=Path("candidate.json")):
            with mock.patch.object(Path, "read_text", return_value=json.dumps(candidate)):
                with self.assertRaisesRegex(status.CandidateStatusError, "distinct"):
                    status.load_candidate(Path("candidate.json"))

    def test_bounded_wait_retries_only_nonterminal_statuses(self):
        reports = [
            {"status": "sequencer-included"},
            {"status": "pending-indexer"},
            {"status": "finalized"},
        ]
        with mock.patch.object(status, "audit", side_effect=reports) as audit:
            with mock.patch.object(status.time, "monotonic", side_effect=[0, 0, 1, 1, 2]):
                with mock.patch.object(status.time, "sleep"):
                    report = status.wait_for_status(self.candidate(), "register:sender", 1, 3, 30, 1)
        self.assertEqual(report["status"], "finalized")
        self.assertEqual(report["attempts"], 3)
        self.assertEqual(audit.call_count, 3)

        with mock.patch.object(
            status, "audit", return_value={"status": "verification-unavailable"}
        ) as audit:
            report = status.wait_for_status(self.candidate(), "register:sender", 1, 3, 30, 1)
        self.assertEqual(report["attempts"], 1)
        self.assertEqual(audit.call_count, 1)


if __name__ == "__main__":
    unittest.main()
