#!/usr/bin/env python3

import base64
import hashlib
import importlib.util
import json
import struct
import unittest
import urllib.error
from argparse import Namespace
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("lez_explorer", REPO / "tools" / "lez_explorer.py")
lez = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lez)


def page(*resources):
    assignments = []
    for index, resource in enumerate(resources):
        assignments.append(
            f"__RESOLVED_RESOURCES[{index}] = {json.dumps(json.dumps(resource))};"
        )
    return "<html><script>" + "".join(assignments) + "</script></html>"


class ExplorerVerifierTests(unittest.TestCase):
    def test_rpc_retries_transport_only_and_rejects_malformed_json(self):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = b'{"result":"ok"}'
        with mock.patch.object(
            lez.urllib.request,
            "urlopen",
            side_effect=[urllib.error.URLError("temporary"), response],
        ) as request:
            with mock.patch.object(lez.time, "sleep") as sleep:
                self.assertEqual(lez.rpc_call("checkHealth", [], 1), "ok")
        self.assertEqual(request.call_count, 2)
        sleep.assert_called_once_with(lez.TRANSPORT_RETRY_SECONDS)

        malformed = mock.MagicMock()
        malformed.__enter__.return_value.read.return_value = b"not-json"
        with mock.patch.object(lez.urllib.request, "urlopen", return_value=malformed) as request:
            with self.assertRaisesRegex(lez.ExplorerValidationError, "malformed JSON"):
                lez.rpc_call("checkHealth", [], 1)
        self.assertEqual(request.call_count, 1)

    def test_explorer_page_retries_transport_only(self):
        response = mock.MagicMock()
        response.__enter__.return_value.geturl.return_value = lez.EXPLORER_URL + "/"
        response.__enter__.return_value.read.return_value = b"explorer"
        with mock.patch.object(
            lez.urllib.request,
            "urlopen",
            side_effect=[urllib.error.URLError("temporary"), response],
        ) as request:
            with mock.patch.object(lez.time, "sleep") as sleep:
                self.assertEqual(lez.fetch_explorer_page("/", 1), "explorer")
        self.assertEqual(request.call_count, 2)
        sleep.assert_called_once_with(lez.TRANSPORT_RETRY_SECONDS)

    def test_hydration_parser_selects_typed_success_resources(self):
        block = {
            "header": {"block_id": 4035, "hash": "60" * 32},
            "body": {"transactions": []},
            "bedrock_status": "Finalized",
        }
        document = page({"Ok": None}, {"Ok": [block]})
        self.assertEqual(lez.explorer_recent_blocks(document), [block])
        self.assertEqual(lez.hydration_resources(document)[0], {"Ok": None})

    def test_http_200_not_found_resource_fails_closed(self):
        document = page({"Err": {"ServerError": "Transaction not found"}})
        with self.assertRaisesRegex(lez.ExplorerValidationError, "not found"):
            lez.explorer_transaction(document, "ab" * 32)

    def test_exact_transaction_hash_is_required(self):
        transaction = {
            "ProgramDeployment": {
                "hash": "ab" * 32,
                "message": {"bytecode": base64.b64encode(b"ELF").decode()},
            }
        }
        with self.assertRaisesRegex(lez.ExplorerValidationError, "omitted"):
            lez.explorer_transaction(page({"Ok": transaction}), "cd" * 32)

    def test_sequencer_block_decodes_pinned_header_and_finality(self):
        raw = (
            struct.pack("<Q", 4035)
            + bytes.fromhex("11" * 32)
            + bytes.fromhex("22" * 32)
            + struct.pack("<Q", 123456)
            + bytes(64)
            + struct.pack("<I", 0)
            + b"\x02"
        )
        block = lez.decode_sequencer_block(base64.b64encode(raw).decode())
        self.assertEqual(block["block_id"], 4035)
        self.assertEqual(block["hash"], "22" * 32)
        self.assertEqual(block["bedrock_status"], "Finalized")

    def test_deployment_decode_checks_borsh_length_and_hash(self):
        bytecode = b"R0BFbonded"
        raw = b"\x02" + struct.pack("<I", len(bytecode)) + bytecode
        transaction = lez.decode_sequencer_transaction(
            base64.b64encode(raw).decode(), 4035
        )
        self.assertEqual(transaction["kind"], "ProgramDeployment")
        self.assertEqual(transaction["bytecode_size"], len(bytecode))
        self.assertEqual(transaction["bytecode_sha256"], hashlib.sha256(bytecode).hexdigest())
        self.assertEqual(transaction["hash"], hashlib.sha256(raw[1:]).hexdigest())
        malformed = raw[:1] + struct.pack("<I", len(bytecode) + 1) + bytecode
        with self.assertRaisesRegex(lez.ExplorerValidationError, "length"):
            lez.decode_sequencer_transaction(base64.b64encode(malformed).decode(), 1)

    def test_public_decode_checks_canonical_serialized_hash(self):
        serialized = b"canonical-public-transaction"
        transaction = lez.decode_sequencer_transaction(
            base64.b64encode(b"\x00" + serialized).decode(), 51
        )
        self.assertEqual(transaction["kind"], "Public")
        self.assertEqual(transaction["block_id"], 51)
        self.assertEqual(transaction["serialized_size"], len(serialized))
        self.assertEqual(transaction["hash"], hashlib.sha256(serialized).hexdigest())

    def test_transaction_account_ids_follow_variant_shape(self):
        public = {"message": {"account_ids": ["sender", "owner"]}}
        private = {
            "message": {
                "public_actions": [
                    {"account_id": "sender"},
                    {"account_id": "owner"},
                ]
            }
        }
        self.assertEqual(lez._transaction_account_ids("Public", public), ["sender", "owner"])
        self.assertEqual(
            lez._transaction_account_ids("PrivacyPreserving", private), ["sender", "owner"]
        )

    def test_program_id_hex_normalizes_to_explorer_base58(self):
        raw = bytes(range(32))
        encoded = lez.base58_encode(raw)
        self.assertEqual(lez.base58_decode(encoded, "program ID"), raw)
        self.assertEqual(lez.explorer_program_id(raw.hex()), encoded)
        self.assertEqual(lez.explorer_program_id(encoded), encoded)
        with self.assertRaisesRegex(lez.ExplorerValidationError, "32 bytes"):
            lez.base58_decode("abc", "account ID")

    def test_generic_public_reconciliation_binds_program_accounts_and_pages(self):
        tx_hash = "ab" * 32
        block_hash = "cd" * 32
        program_id_hex = "ef" * 32
        program_id = lez.base58_encode(bytes.fromhex(program_id_hex))
        sender = lez.base58_encode(b"\x01" * 32)
        context = {
            "observed_at": "2026-08-12T00:00:00+00:00",
            "channel_id": "test-channel",
            "head": {"block_id": 55},
            "explorer_tip": {
                "header": {"block_id": 55, "hash": "12" * 32, "timestamp": 42},
                "bedrock_status": "Finalized",
            },
            "explorer_tip_id": 55,
            "overlaps": [{"matches": True}] * 3,
            "newest_sequencer_finalized": 55,
            "lag_scan_complete": True,
            "sequencer_tx": {
                "kind": "Public",
                "block_id": 51,
                "hash": tx_hash,
                "serialized_size": 123,
            },
            "sequencer_block": {
                "block_id": 51,
                "hash": block_hash,
                "bedrock_status": "Finalized",
            },
            "block_path": "/block/51",
            "tx_path": f"/transaction/{tx_hash}",
            "block_document": f"51 {block_hash} {tx_hash} Finalized",
            "transaction_document": f"{tx_hash} Public Transaction {program_id} {sender}",
            "indexed_block": {
                "header": {"block_id": 51, "hash": block_hash},
                "bedrock_status": "Finalized",
            },
            "indexed_kind": "Public",
            "indexed_payload": {
                "hash": tx_hash,
                "message": {"program_id": program_id, "account_ids": [sender]},
            },
            "indexed_transactions": [{"kind": "Public", "hash": tx_hash}],
            "confirmations": 4,
        }
        args = Namespace(
            tx_hash=tx_hash,
            block_id=51,
            transaction_type="Public",
            kind="wallet-registration",
            component="testnet-wallet",
            operation="register-sender",
            program_id=program_id_hex,
            account_id=[sender],
            verifier_commit="",
            observer="primary",
            confirmations=3,
            overlap_count=3,
            lag_scan=32,
            evidence=None,
        )
        with mock.patch.object(lez, "_collect_reconciliation", return_value=context):
            report = lez.reconcile_transaction(args)
        self.assertEqual(report["status"], "finalized")
        self.assertEqual(report["public_program_id"], program_id)
        self.assertTrue(all(report["checks"].values()))

        wrong_account = lez.base58_encode(b"\x02" * 32)
        args.account_id = [wrong_account]
        context["transaction_document"] += f" {wrong_account}"
        with mock.patch.object(lez, "_collect_reconciliation", return_value=context):
            report = lez.reconcile_transaction(args)
        self.assertEqual(report["status"], "disputed")
        self.assertFalse(report["checks"]["expected_accounts_match"])

    def test_rendered_not_found_and_missing_expected_text_fail(self):
        with self.assertRaisesRegex(lez.ExplorerValidationError, "not found"):
            lez._require_rendered("Transaction not found", ["hash"], "transaction")
        with self.assertRaisesRegex(lez.ExplorerValidationError, "did not render"):
            lez._require_rendered("Finalized", ["expected-hash"], "block")


if __name__ == "__main__":
    unittest.main()
