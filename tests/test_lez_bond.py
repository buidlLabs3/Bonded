#!/usr/bin/env python3

import importlib.util
import ctypes
import tempfile
import types
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("lez_bond", REPO / "tools" / "lez_bond.py")
bond = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bond)


class BondWalletAdapterTests(unittest.TestCase):
    def test_ffi_account_encoder_uses_the_header_pointer_abi(self):
        output = ctypes.create_string_buffer(b"11111111111111111111111111111111")
        captured = []

        def encode(pointer):
            captured.append(pointer)
            return ctypes.addressof(output)

        adapter = bond.OfficialWalletFfi.__new__(bond.OfficialWalletFfi)
        adapter.lib = types.SimpleNamespace(
            wallet_ffi_account_id_to_base58=encode,
            wallet_ffi_free_string=lambda _value: None,
        )
        value = bond.FfiBytes32.from_bytes(bytes(32))
        self.assertEqual(adapter.display_account(value), output.value.decode("ascii"))
        decoded = ctypes.cast(captured[0], ctypes.POINTER(bond.FfiBytes32)).contents
        self.assertEqual(decoded.as_bytes(), bytes(32))

    def test_program_ids_round_trip_in_canonical_little_endian_words(self):
        value = bond.program_id_from_hex(bond.CANONICAL_PROGRAM_ID)
        self.assertEqual(bond.program_id_to_hex(value), bond.CANONICAL_PROGRAM_ID)
        self.assertEqual(
            bond.program_id_to_hex(bond.program_id_from_hex(bond.AUTHENTICATED_TRANSFER_PROGRAM_ID)),
            bond.AUTHENTICATED_TRANSFER_PROGRAM_ID,
        )

    def test_initialize_instruction_vector_is_stable(self):
        args = types.SimpleNamespace(
            bond_id="01" * 32,
            message_commitment="02" * 32,
            policy_commitment="03" * 32,
            amount=0x112233445566778899AABBCCDDEEFF00,
            deadline_ms=0x0102030405060708,
        )
        accounts = {
            "sender": bond.FfiBytes32.from_bytes(bytes([4]) * 32),
            "owner": bond.FfiBytes32.from_bytes(bytes([5]) * 32),
            "sink": bond.FfiBytes32.from_bytes(bytes([6]) * 32),
        }
        words = bond.initialize_words(args, accounts)
        self.assertEqual(len(words), 199)
        self.assertEqual(words[:4], [0, 1, 1, 1])
        self.assertEqual(words[-6:], [0xDDEEFF00, 0x99AABBCC, 0x55667788, 0x11223344, 0x05060708, 0x01020304])
        self.assertEqual(
            bond.instruction_digest(words),
            "acb9048308c5744acdb1ce5f811245e628856d3ddaf167aecc4a8f01ba191172",
        )

    def test_settlement_vectors_match_enum_order(self):
        self.assertEqual(bond.settle_words("refund-accepted"), [1, 0])
        self.assertEqual(bond.settle_words("sink-rejected"), [1, 1])
        self.assertEqual(bond.settle_words("refund-expired"), [1, 2])
        self.assertEqual(bond.settle_words("refund-delivery-failed"), [1, 3])

    def test_state_seed_matches_the_guest_digest_vector(self):
        self.assertEqual(
            bond.hashlib.sha256(bond.STATE_SEED_DOMAIN + bytes([7]) * 32).hexdigest(),
            "da2a0a77e7198ba0740ecc165c2ca59a4ee26f1b7ce1110babd09df2ab6baa13",
        )

    def test_native_output_is_scanned_before_temporary_cleanup(self):
        with bond.captured_native_output() as captured:
            pass
        self.assertRegex(captured["captured_output_sha256"], r"^[0-9a-f]{64}$")
        self.assertGreaterEqual(captured["captured_output_bytes"], 0)

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "wallet.log"
            path.write_text("Recovery phrase: should never escape", encoding="utf-8")
            with self.assertRaisesRegex(bond.BondAdapterError, "secret"):
                bond.scan_native_log(path)
            path.write_text("Nullifier secret: should never escape", encoding="utf-8")
            with self.assertRaisesRegex(bond.BondAdapterError, "secret"):
                bond.scan_native_log(path)

    def test_initialize_and_settlement_relationships_fail_closed(self):
        args = types.SimpleNamespace(amount=25)
        before = {
            "sender": {"balance": "100"},
            "escrow": {"balance": "0", "program_owner": "00" * 32},
            "state": {"data_size": 0, "data_sha256": "a"},
        }
        after = {
            "sender": {"balance": "75"},
            "escrow": {
                "balance": "25",
                "program_owner": bond.AUTHENTICATED_TRANSFER_PROGRAM_ID,
            },
            "state": {
                "data_size": 1,
                "data_sha256": "b",
                "program_owner": bond.CANONICAL_PROGRAM_ID,
            },
        }
        bond._validate_relationships("initialize", args, before, after)
        after["sender"]["balance"] = "76"
        with self.assertRaisesRegex(bond.BondAdapterError, "sender balance"):
            bond._validate_relationships("initialize", args, before, after)

        before = {
            "escrow": {"balance": "25"},
            "destination": {"balance": "10"},
            "owner": {"balance": "7"},
            "state": {"data_sha256": "a"},
        }
        after = {
            "escrow": {"balance": "0"},
            "destination": {"balance": "35"},
            "owner": {"balance": "7"},
            "state": {"data_sha256": "b"},
        }
        bond._validate_relationships("settle", args, before, after)
        after["owner"]["balance"] = "8"
        with self.assertRaisesRegex(bond.BondAdapterError, "owner"):
            bond._validate_relationships("settle", args, before, after)


if __name__ == "__main__":
    unittest.main()
