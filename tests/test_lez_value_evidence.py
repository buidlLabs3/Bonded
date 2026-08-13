#!/usr/bin/env python3

import importlib.util
import json
import tempfile
import types
import unittest
import os
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_value_evidence", REPO / "tools" / "lez_value_evidence.py"
)
evidence = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evidence)


class ValueEvidenceTests(unittest.TestCase):
    def fixture(self, root: Path):
        payload = {
            "operation": "below-limit-transfer",
            "network": "lez-testnet",
            "profile": "settlement",
            "sender": "sender",
            "recipient": "recipient",
            "amount": 2,
            "nonce": "12" * 32,
            "created_at_ms": 1000,
            "expires_at_ms": 3000,
            "claims": {
                "decision": "autonomous-below-limit",
                "policy": {"per_transaction": 2, "per_period": 6},
            },
        }
        private = os.urandom(32)
        attestation = {
            "role": "policy-owner",
            "public_key": evidence.lez_value.public_key_from_private(private).hex(),
            "signature": evidence.lez_value.sign_message(
                private, evidence.lez_value.canonical_payload(payload)
            ).hex(),
        }
        authorization = {
            "schema_version": 1,
            "payload": payload,
            "payload_sha256": evidence.lez_value.payload_digest(payload),
            "attestations": [attestation],
        }
        authorization_path = root / "authorization.json"
        authorization_path.write_text(json.dumps(authorization), encoding="utf-8")
        trusted_path = root / "trusted.json"
        trusted_path.write_text(
            json.dumps({"policy-owner": attestation["public_key"]}), encoding="utf-8"
        )
        candidate = {
            "status": "official-wallet-sequencer-finalized-candidate",
            "operation": "below-limit-transfer",
            "network": "lez-testnet",
            "program_id": evidence.lez_value.AUTHENTICATED_TRANSFER_PROGRAM_ID,
            "transaction": "ab" * 32,
            "transaction_type": "PrivacyPreserving",
            "block": 42,
            "block_hash": "cd" * 32,
            "finality": "Finalized",
            "profile": "settlement",
            "accounts": {"sender": "sender", "recipient": "recipient"},
            "amount": 2,
            "authorization": {"payload_sha256": "12" * 32},
            "authorization_sha256": "34" * 32,
            "trusted_signers_sha256": "78" * 32,
            "instruction_words_sha256": "56" * 32,
            "state": {
                "before": {
                    "sender": {"balance": "10"},
                    "recipient": {"balance": "1"},
                },
                "after": {
                    "sender": {"balance": "8"},
                    "recipient": {"balance": "3"},
                },
            },
        }
        candidate["authorization_sha256"] = evidence.hashlib.sha256(
            authorization_path.read_bytes()
        ).hexdigest()
        candidate["trusted_signers_sha256"] = evidence.hashlib.sha256(
            trusted_path.read_bytes()
        ).hexdigest()
        candidate_path = root / "candidate.json"
        candidate_path.write_text(json.dumps(candidate), encoding="utf-8")
        return types.SimpleNamespace(
            candidate=candidate_path,
            authorization=authorization_path,
            trusted_signers=trusted_path,
            operation="below-limit-transfer",
            verifier_commit="78" * 20,
            observer="primary",
            confirmations=3,
            evidence=root / "result.json",
        )

    def reconciled(self):
        return {
            "status": "finalized",
            "transaction": "ab" * 32,
            "transaction_type": "PrivacyPreserving",
            "block": 42,
            "block_hash": "cd" * 32,
            "checks": {"all": True},
        }

    def test_promotion_binds_exact_sender_and_recipient(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(Path(directory))
            with mock.patch.object(
                evidence.lez_explorer, "reconcile_transaction", return_value=self.reconciled()
            ) as reconcile:
                result = evidence.promote(args)
            called = reconcile.call_args.args[0]
            self.assertEqual(called.account_id, ["sender", "recipient"])
            self.assertEqual(called.kind, "spending-control")
            self.assertEqual(result["value_transfer_provenance"]["amount"], 2)

    def test_nonfinal_and_explorer_conflicts_write_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.fixture(root)
            candidate = json.loads(args.candidate.read_text(encoding="utf-8"))
            candidate["finality"] = "Pending"
            args.candidate.write_text(json.dumps(candidate), encoding="utf-8")
            with self.assertRaisesRegex(evidence.ValueEvidenceError, "canonical finalized"):
                evidence.promote(args)
            self.assertFalse(args.evidence.exists())
            candidate["finality"] = "Finalized"
            args.candidate.write_text(json.dumps(candidate), encoding="utf-8")
            result = self.reconciled()
            result["block_hash"] = "ef" * 32
            with mock.patch.object(
                evidence.lez_explorer, "reconcile_transaction", return_value=result
            ):
                with self.assertRaisesRegex(evidence.ValueEvidenceError, "conflicts"):
                    evidence.promote(args)
            self.assertFalse(args.evidence.exists())


if __name__ == "__main__":
    unittest.main()
