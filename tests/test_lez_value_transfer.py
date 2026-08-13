#!/usr/bin/env python3

import importlib.util
import json
import os
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_value_transfer", REPO / "tools" / "lez_value_transfer.py"
)
value = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(value)


class ValueTransferTests(unittest.TestCase):
    def payload(self, operation="below-limit-transfer", amount=2):
        claims = {
            "decision": "autonomous-below-limit",
            "policy": {"per_transaction": 2, "per_period": 6},
        }
        if operation == "owner-approved-transfer":
            claims = {
                "decision": "owner-approved",
                "proposal_id": "approval-1",
                "policy": {"per_transaction": 2, "per_period": 6},
            }
        elif operation == "paid-task-settlement":
            claims = {
                "task_id": "task-1",
                "requester": "agent:requester",
                "provider": "agent:provider",
                "skill": "storage.audit",
                "task_state": "completed",
            }
        return {
            "operation": operation,
            "network": "lez-testnet",
            "profile": "settlement",
            "sender": "sender",
            "recipient": "recipient",
            "amount": amount,
            "nonce": "12" * 32,
            "created_at_ms": 1000,
            "expires_at_ms": 3000,
            "claims": claims,
        }

    def signed(self, payload, roles):
        attestations = []
        message = value.canonical_payload(payload)
        for role in roles:
            private = os.urandom(32)
            attestations.append(
                {
                    "role": role,
                    "public_key": value.public_key_from_private(private).hex(),
                    "signature": value.sign_message(private, message).hex(),
                }
            )
        return {
            "schema_version": 1,
            "payload": payload,
            "payload_sha256": value.payload_digest(payload),
            "attestations": attestations,
        }

    def test_transfer_instruction_vector_matches_risc0_serde_layout(self):
        words = value.transfer_words(0x112233445566778899AABBCCDDEEFF00)
        self.assertEqual(
            words,
            [0, 0xDDEEFF00, 0x99AABBCC, 0x55667788, 0x11223344],
        )
        self.assertEqual(
            value.instruction_digest(words),
            "bfb0ad1618b14a712e9c1c58757bc4528ce6689a3e0913ba145e98573c2cf7dc",
        )

    def test_below_limit_requires_exact_policy_role_and_current_window(self):
        document = self.signed(self.payload(), ["policy-owner"])
        self.assertEqual(value.validate_authorization(document, 2000)["amount"], 2)
        document["payload"]["amount"] = 3
        document["payload_sha256"] = value.payload_digest(document["payload"])
        with self.assertRaisesRegex(value.ValueTransferError, "below-limit"):
            value.validate_authorization(document, 2000)
        document = self.signed(self.payload(), ["owner"])
        with self.assertRaisesRegex(value.ValueTransferError, "signer roles"):
            value.validate_authorization(document, 2000)
        document = self.signed(self.payload(), ["policy-owner"])
        with self.assertRaisesRegex(value.ValueTransferError, "currently valid"):
            value.validate_authorization(document, 4000)

    def test_owner_approval_must_be_above_limit_and_signature_bound(self):
        payload = self.payload("owner-approved-transfer", 3)
        document = self.signed(payload, ["owner"])
        value.validate_authorization(document, 2000)
        document["payload"]["recipient"] = "tampered"
        document["payload_sha256"] = value.payload_digest(document["payload"])
        with self.assertRaisesRegex(value.ValueTransferError, "signature"):
            value.validate_authorization(document, 2000)
        payload = self.payload("owner-approved-transfer", 2)
        document = self.signed(payload, ["owner"])
        with self.assertRaisesRegex(value.ValueTransferError, "above-limit"):
            value.validate_authorization(document, 2000)

    def test_payload_digest_and_transfer_program_id_are_bound(self):
        document = self.signed(self.payload(), ["policy-owner"])
        document["payload_sha256"] = "00" * 32
        with self.assertRaisesRegex(value.ValueTransferError, "payload digest"):
            value.validate_authorization(document, 2000)
        completed = types.SimpleNamespace(stdout=value.AUTHENTICATED_TRANSFER_PROGRAM_ID + "\n")
        with mock.patch.object(value.subprocess, "run", return_value=completed):
            result = value.inspect_transfer_program(b"guest")
        self.assertEqual(result["program_id"], value.AUTHENTICATED_TRANSFER_PROGRAM_ID)
        completed.stdout = "ff" * 32
        with mock.patch.object(value.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(value.ValueTransferError, "canonical ID"):
                value.inspect_transfer_program(b"guest")

    def test_paid_task_requires_requester_and_provider(self):
        payload = self.payload("paid-task-settlement", 4)
        document = self.signed(payload, ["requester", "provider"])
        value.validate_authorization(document, 2000)
        document = self.signed(payload, ["requester"])
        with self.assertRaisesRegex(value.ValueTransferError, "signer roles"):
            value.validate_authorization(document, 2000)

    def test_execution_pins_roles_to_trusted_public_keys(self):
        document = self.signed(self.payload(), ["policy-owner"])
        trusted = {"policy-owner": document["attestations"][0]["public_key"]}
        value.validate_authorization(document, 2000, trusted)
        trusted["policy-owner"] = "ff" * 32
        with self.assertRaisesRegex(value.ValueTransferError, "not pinned"):
            value.validate_authorization(document, 2000, trusted)
        with self.assertRaisesRegex(value.ValueTransferError, "not pinned"):
            value.validate_authorization(document, 2000, {})

    def test_interrupted_submission_fails_closed_and_does_not_retry(self):
        inspection = {field: field for field in value.RESUME_FIELDS}
        candidate = {**inspection, "status": "submitting"}
        value._validate_resume(candidate, inspection)
        candidate["status"] = "untrusted"
        with self.assertRaisesRegex(value.ValueTransferError, "unsupported status"):
            value._validate_resume(candidate, inspection)

    def test_signing_key_permissions_are_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = self.payload()
            authorization = root / "authorization.json"
            authorization.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "payload": payload,
                        "payload_sha256": value.payload_digest(payload),
                        "attestations": [],
                    }
                ),
                encoding="utf-8",
            )
            key = root / "key"
            key.write_text(os.urandom(32).hex(), encoding="ascii")
            key.chmod(0o644)
            args = types.SimpleNamespace(
                authorization=authorization,
                private_key=key,
                role="policy-owner",
                evidence=root / "signed.json",
            )
            with self.assertRaisesRegex(value.ValueTransferError, "private regular"):
                value.sign_authorization(args)
            symlink = root / "key-link"
            symlink.symlink_to(key)
            args.private_key = symlink
            with self.assertRaisesRegex(value.ValueTransferError, "private regular"):
                value.sign_authorization(args)
            args.private_key = key
            key.chmod(0o600)
            signed = value.sign_authorization(args)
            self.assertEqual(signed["attestations"][0]["role"], "policy-owner")
            value.validate_authorization(signed, 2000)


if __name__ == "__main__":
    unittest.main()
