#!/usr/bin/env python3

import base64
import hashlib
import importlib.util
import json
import struct
import unittest
from pathlib import Path


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

    def test_rendered_not_found_and_missing_expected_text_fail(self):
        with self.assertRaisesRegex(lez.ExplorerValidationError, "not found"):
            lez._require_rendered("Transaction not found", ["hash"], "transaction")
        with self.assertRaisesRegex(lez.ExplorerValidationError, "did not render"):
            lez._require_rendered("Finalized", ["expected-hash"], "block")


if __name__ == "__main__":
    unittest.main()
