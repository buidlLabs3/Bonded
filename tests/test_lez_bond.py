#!/usr/bin/env python3

import importlib.util
import ctypes
import json
import os
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


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

    def test_finality_poll_retries_transient_rpc_errors(self):
        transaction = "ab" * 32
        raw = bytes([0]) + b"public-transaction"
        transaction = bond.hashlib.sha256(raw[1:]).hexdigest()
        responses = [
            bond.lez_explorer.ExplorerTransportError("temporary network error"),
            [bond.base64.b64encode(raw).decode("ascii"), 9],
            bond.base64.b64encode(bytes(148) + bytes([2])).decode("ascii"),
        ]

        def rpc(_method, _params):
            response = responses.pop(0)
            if isinstance(response, Exception):
                raise response
            return response

        with mock.patch.object(bond.lez_explorer, "rpc_call", side_effect=rpc):
            with mock.patch.object(bond.time, "sleep"):
                result = bond.wait_for_finalized(transaction, 1, "Public")
        self.assertEqual(result["finality"], "Finalized")
        self.assertEqual(result["transaction_type"], "Public")

    def test_finality_poll_reports_bounded_transient_failure(self):
        clock = iter([0.0, 0.1, 2.0])
        with mock.patch.object(
            bond.lez_explorer,
            "rpc_call",
            side_effect=bond.lez_explorer.ExplorerTransportError("network unavailable"),
        ):
            with mock.patch.object(bond.time, "monotonic", side_effect=lambda: next(clock)):
                with mock.patch.object(bond.time, "sleep"):
                    with self.assertRaisesRegex(
                        bond.BondAdapterError,
                        "last RPC error network unavailable",
                    ):
                        bond.wait_for_finalized("ab" * 32, 1, "Public")

    def test_finality_poll_does_not_retry_semantic_failure(self):
        with mock.patch.object(
            bond.lez_explorer,
            "rpc_call",
            side_effect=bond.lez_explorer.ExplorerValidationError("malformed response"),
        ) as rpc:
            with self.assertRaisesRegex(
                bond.lez_explorer.ExplorerValidationError, "malformed response"
            ):
                bond.wait_for_finalized("ab" * 32, 30, "Public")
        self.assertEqual(rpc.call_count, 1)

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

    def test_timeout_resume_observes_original_hash_without_resubmission(self):
        class FakeFfi:
            submit_calls = 0
            finalized = False

            def __init__(self, _provenance, _wallet):
                pass

            def close(self):
                pass

            def account(self, value):
                index = {"sender": 1, "owner": 2, "sink": 3}[value]
                return bond.FfiBytes32.from_bytes(bytes([index]) * 32)

            def pda(self, _program, domain, _bond_id):
                index = 4 if domain == bond.STATE_SEED_DOMAIN else 5
                return bond.FfiBytes32.from_bytes(bytes([index]) * 32)

            def display_account(self, value):
                raw = value.as_bytes()
                names = {
                    bytes([1]) * 32: "sender",
                    bytes([2]) * 32: "owner",
                    bytes([3]) * 32: "sink",
                    bytes([4]) * 32: "state",
                    bytes([5]) * 32: "escrow",
                }
                return names[raw]

            def snapshot(self, value):
                name = self.display_account(value)
                before = {
                    "sender": ("100", "00" * 32, 0, "a"),
                    "owner": ("7", "00" * 32, 0, "a"),
                    "sink": ("0", "00" * 32, 0, "a"),
                    "state": ("0", "00" * 32, 0, "a"),
                    "escrow": ("0", "00" * 32, 0, "a"),
                }
                after = {
                    **before,
                    "sender": ("75", "00" * 32, 0, "a"),
                    "state": ("0", bond.CANONICAL_PROGRAM_ID, 1, "b"),
                    "escrow": (
                        "25",
                        bond.AUTHENTICATED_TRANSFER_PROGRAM_ID,
                        0,
                        "a",
                    ),
                }
                balance, owner, data_size, digest = (
                    after if self.finalized else before
                )[name]
                return {
                    "account_id": name,
                    "program_owner": owner,
                    "balance": balance,
                    "nonce": "0",
                    "data_size": data_size,
                    "data_sha256": digest,
                }

            def submit(self, _ordered, _words, _elf):
                type(self).submit_calls += 1
                return "ab" * 32

        profile = {
            "network": "lez-testnet",
            "channel_id": "01" * 32,
            "lez_release": "v0.2.4",
            "release_commit": "upstream-commit",
        }
        provenance = {
            "source_commit": "upstream-commit",
            "ffi_header_sha256": "11" * 32,
            "ffi_library_sha256": "22" * 32,
        }
        program = {
            "program_id": bond.CANONICAL_PROGRAM_ID,
            "binary_sha256": "33" * 32,
            "binary_size": 10,
            "binary": "unused.bin",
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile_path = root / "profile.json"
            elf = root / "guest.bin"
            evidence_path = root / "candidate.json"
            profile_path.write_text("{}", encoding="utf-8")
            elf.write_bytes(b"guest")
            args = types.SimpleNamespace(
                profile=profile_path,
                wallet_source=root,
                wallet_home=root,
                elf=elf,
                program_id=bond.CANONICAL_PROGRAM_ID,
                sender="sender",
                owner="owner",
                sink="sink",
                bond_id="44" * 32,
                operation="initialize",
                message_commitment="55" * 32,
                policy_commitment="66" * 32,
                amount=25,
                deadline_ms=1234,
                submit=True,
                timeout=1,
                evidence=evidence_path,
            )
            inclusion = {
                "transaction": "ab" * 32,
                "transaction_type": "PrivacyPreserving",
                "serialized_transaction_sha256": "ab" * 32,
                "serialized_transaction_bytes": 100,
                "block": 9,
                "block_hash": "cd" * 32,
                "finality": "Finalized",
            }

            def finalize(_transaction, _timeout):
                FakeFfi.finalized = True
                return inclusion

            patches = (
                mock.patch.object(bond.lez_wallet, "load_network_profile", return_value=profile),
                mock.patch.object(bond.lez_wallet, "verify_source", return_value=provenance),
                mock.patch.object(bond.lez_wallet, "verify_wallet_home", return_value={}),
                mock.patch.object(bond, "verify_program", return_value=program),
                mock.patch.object(bond, "OfficialWalletFfi", FakeFfi),
            )
            with patches[0], patches[1], patches[2], patches[3], patches[4]:
                with mock.patch.object(
                    bond,
                    "wait_for_finalized",
                    side_effect=bond.BondAdapterError("timed out"),
                ):
                    with mock.patch.dict(
                        os.environ,
                        {"BONDED_LEZ_SUBMIT": "YES", "RISC0_DEV_MODE": "0"},
                    ):
                        with self.assertRaisesRegex(bond.BondAdapterError, "timed out"):
                            bond.execute(args)
                submitted = json.loads(evidence_path.read_text(encoding="utf-8"))
                self.assertEqual(submitted["status"], "submitted")
                self.assertEqual(submitted["transaction"], "ab" * 32)
                self.assertEqual(FakeFfi.submit_calls, 1)

                with mock.patch.object(bond, "wait_for_finalized", side_effect=finalize):
                    with mock.patch.dict(
                        os.environ,
                        {"BONDED_LEZ_SUBMIT": "YES", "RISC0_DEV_MODE": "0"},
                    ):
                        completed = bond.execute(args)
                self.assertEqual(FakeFfi.submit_calls, 1)
                self.assertEqual(
                    completed["status"],
                    "official-wallet-sequencer-finalized-candidate",
                )
                self.assertEqual(completed["transaction"], "ab" * 32)

    def test_resume_rejects_any_changed_call_binding(self):
        inspection = {field: field for field in bond.RESUME_BINDING_FIELDS}
        candidate = {
            **inspection,
            "status": "submitted",
            "transaction": "ab" * 32,
            "state": {"before": {}},
        }
        bond._validate_resume(candidate, inspection)
        candidate["bond_id"] = "different"
        with self.assertRaisesRegex(bond.BondAdapterError, "bond_id"):
            bond._validate_resume(candidate, inspection)


if __name__ == "__main__":
    unittest.main()
