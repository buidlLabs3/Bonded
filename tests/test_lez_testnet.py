#!/usr/bin/env python3

import base64
import hashlib
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("lez_testnet", REPO / "tools" / "lez_testnet.py")
lez = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lez)


class TestnetClientTests(unittest.TestCase):
    def test_deployment_encoding_and_hash_match_official_borsh_shape(self):
        for program in (b"\x7fELFbonded", b"R0BFbonded"):
            with self.subTest(magic=program[:4]):
                payload = base64.b64decode(lez.deployment_payload(program))
                self.assertEqual(
                    payload, b"\x02" + struct.pack("<I", len(program)) + program
                )
                expected = hashlib.sha256(
                    struct.pack("<I", len(program)) + program
                ).hexdigest()
                self.assertEqual(lez.deployment_transaction_hash(program), expected)

    def test_invalid_program_and_insecure_endpoint_fail_closed(self):
        with self.assertRaises(lez.TestnetError):
            lez.deployment_payload(b"not-a-risc0-program")
        with self.assertRaises(lez.TestnetError):
            lez.require_endpoint("http://example.test", False)
        lez.require_endpoint("http://127.0.0.1:3040", True)
        with self.assertRaises(lez.TestnetError):
            lez.require_endpoint("https://user:secret@example.test", False)

    def test_preflight_requires_exact_official_program_ids(self):
        with mock.patch.object(
            lez,
            "rpc_call",
            side_effect=[None, 17, {"authenticated_transfer": [1] * 8}],
        ):
            with self.assertRaisesRegex(lez.TestnetError, "privacy_preserving_circuit"):
                lez.preflight(lez.DEFAULT_ENDPOINT, False)
        programs = dict(lez.EXPECTED_PROGRAMS)
        programs["authenticated_transfer"] = [0] * 8
        with mock.patch.object(
            lez,
            "rpc_call",
            side_effect=[None, 17, programs],
        ):
            with self.assertRaisesRegex(lez.TestnetError, "authenticated_transfer"):
                lez.preflight(lez.DEFAULT_ENDPOINT, False)

    def test_atomic_evidence_contains_no_private_material(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            value = {"status": "verified", "transaction": "ab" * 32}
            lez.atomic_json(path, value)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), value)
            text = path.read_text(encoding="utf-8").lower()
            self.assertNotIn("mnemonic", text)
            self.assertNotIn("private_key", text)


if __name__ == "__main__":
    unittest.main()
