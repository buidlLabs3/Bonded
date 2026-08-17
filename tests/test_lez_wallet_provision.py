#!/usr/bin/env python3

import contextlib
import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_wallet_provision", REPO / "tools" / "lez_wallet_provision.py"
)
provision = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provision)


class WalletProvisionTests(unittest.TestCase):
    def test_account_manifest_is_private_complete_and_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            manifest = home / "public-accounts.json"
            manifest.write_text(
                json.dumps({"accounts": {"sender": "a", "owner": "b", "sink": "c"}}),
                encoding="utf-8",
            )
            manifest.chmod(0o600)
            self.assertEqual(provision.load_public_accounts(home)["sender"], "a")
            manifest.chmod(0o644)
            with self.assertRaisesRegex(provision.ProvisionError, "0600"):
                provision.load_public_accounts(home)
            manifest.chmod(0o600)
            manifest.write_text(
                json.dumps({"accounts": {"sender": "a", "owner": "a", "sink": "c"}}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(provision.ProvisionError, "distinct"):
                provision.load_public_accounts(home)

    def test_agent_manifest_binds_profile_kind_and_both_account_encodings(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            account_hex = bytes(range(32)).hex()
            account_base58 = provision.lez_bond.lez_explorer.base58_encode(
                bytes.fromhex(account_hex)
            )
            manifest = home / "agent-account.json"
            manifest.write_text(json.dumps({
                "agent": {
                    "profile": "inbox",
                    "kind": "private-owned",
                    "id_hex": account_hex,
                    "id_base58": account_base58,
                }
            }), encoding="utf-8")
            manifest.chmod(0o600)
            account = provision.load_agent_account(home, "inbox")
            self.assertEqual(account["id_hex"], account_hex)
            with self.assertRaisesRegex(provision.ProvisionError, "profile or kind"):
                provision.load_agent_account(home, "vault")
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["agent"]["id_base58"] = "wrong"
            manifest.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(provision.ProvisionError, "do not match"):
                provision.load_agent_account(home, "inbox")

    def test_parser_keeps_public_and_private_modes_explicit(self):
        agent = provision.parser().parse_args([
            "--wallet-source", "/tmp/source",
            "--wallet-home", "/tmp/wallet",
            "--agent-profile", "vault",
            "initialize-agent",
        ])
        self.assertEqual(agent.operation, "initialize-agent")
        self.assertEqual(agent.agent_profile, "vault")
        self.assertIsNone(agent.role)

    def test_private_agent_submission_is_journaled_and_finalized(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory) / "wallet"
            home.mkdir()
            account_hex = bytes(range(32)).hex()
            account_base58 = provision.lez_bond.lez_explorer.base58_encode(
                bytes.fromhex(account_hex)
            )
            manifest = home / "agent-account.json"
            manifest.write_text(json.dumps({
                "agent": {
                    "profile": "vault",
                    "kind": "private-owned",
                    "id_hex": account_hex,
                    "id_base58": account_base58,
                }
            }), encoding="utf-8")
            manifest.chmod(0o600)
            evidence_path = Path(directory) / "evidence.json"

            class FakeWallet:
                closed = False

                def __init__(self, _provenance, _wallet):
                    self.private_queries = 0
                    self.pinata_queries = 0

                def private_snapshot(self, account):
                    self.private_queries += 1
                    return {
                        "account_id": account.as_bytes().hex(),
                        "program_owner": (
                            "00" * 32 if self.private_queries == 1
                            else provision.lez_bond.AUTHENTICATED_TRANSFER_PROGRAM_ID
                        ),
                        "balance": "150" if self.private_queries == 3 else "0",
                        "nonce": "0",
                        "data_size": 0,
                        "data_sha256": hashlib.sha256(b"").hexdigest(),
                    }

                def account(self, value):
                    return value

                def snapshot(self, _account):
                    self.pinata_queries += 1
                    return {"balance": "1000" if self.pinata_queries == 1 else "850"}

                def account_data(self, _account):
                    return bytes([1]) + bytes(32)

                def register_private(self, _account):
                    return "aa" * 32

                def sync_private_to_block(self, _block):
                    return {
                        "captured_output_sha256": "ce" * 32,
                        "captured_output_bytes": 0,
                        "error_categories": [],
                    }

                def claim_private_initialized(self, _account, _solution):
                    return "ab" * 32

                def close(self):
                    self.closed = True

            @contextlib.contextmanager
            def captured_output():
                yield {"captured_output_sha256": "cd" * 32, "captured_output_bytes": 0}

            args = SimpleNamespace(
                submit=True,
                profile=REPO / "config/lez-testnet-network.json",
                wallet_source=Path(directory) / "source",
                wallet_home=home,
                agent_profile="vault",
                evidence=evidence_path,
                timeout=10,
                prover="ipc",
                rayon_threads=2,
                operation="initialize-agent",
            )
            with mock.patch.dict(os.environ, {
                "BONDED_LEZ_SUBMIT": "YES",
                "RISC0_DEV_MODE": "0",
            }), mock.patch.object(
                provision.lez_wallet, "verify_source",
                return_value={"source_commit": provision.lez_wallet.OFFICIAL_RELEASE_COMMIT},
            ), mock.patch.object(
                provision.lez_wallet, "verify_wallet_home", return_value={"home": str(home)},
            ), mock.patch.object(
                provision, "ProvisionWallet", FakeWallet,
            ), mock.patch.object(
                provision, "solve_pinata", return_value=(7, {"solution_sha256": "ef" * 32}),
            ), mock.patch.object(
                provision.lez_bond, "captured_native_output", captured_output,
            ), mock.patch.object(
                provision.lez_bond, "wait_for_finalized",
                return_value={"block": 123, "confirmation_depth": 3},
            ):
                evidence = provision.execute_agent(args)

            self.assertEqual(evidence["status"], "provisioned")
            self.assertEqual(evidence["execution"]["rayon_num_threads"], 2)
            operation = evidence["operations"][0]
            self.assertEqual(operation["status"], "finalized")
            self.assertEqual(operation["registration"]["status"], "finalized")
            self.assertEqual(operation["registration"]["transaction"], "aa" * 32)
            self.assertEqual(operation["registration"]["private_sync"]["captured_output_bytes"], 0)
            self.assertEqual(operation["transaction"], "ab" * 32)
            self.assertEqual(operation["private_sync"]["captured_output_bytes"], 0)
            self.assertEqual(operation["block"], 123)
            self.assertEqual(json.loads(evidence_path.read_text()), evidence)

    def test_pinata_solver_matches_pinned_guest_rule(self):
        data = bytes([1]) + bytes(range(32))
        solution, evidence = provision.solve_pinata(data)
        digest = hashlib.sha256(data[1:] + solution.to_bytes(16, "little")).digest()
        self.assertEqual(digest[0], 0)
        self.assertEqual(evidence["difficulty_zero_bytes"], 1)
        self.assertRegex(evidence["solution_sha256"], r"^[0-9a-f]{64}$")

    def test_candidate_resume_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.json"
            self.assertEqual(provision._candidate(path)["status"], "provisioning-in-progress")
            path.write_text('{"status":"verified"}', encoding="utf-8")
            with self.assertRaisesRegex(provision.ProvisionError, "unsupported"):
                provision._candidate(path)

    def test_operation_journal_rejects_duplicate_ids(self):
        evidence = {"operations": [{"id": "register:sender"}, {"id": "register:sender"}]}
        with self.assertRaisesRegex(provision.ProvisionError, "duplicate"):
            provision._operation(evidence, "register:sender")

    def test_submitting_is_an_explicit_ambiguous_state(self):
        evidence = {"operations": [{"id": "fund:sender", "status": "submitting"}]}
        self.assertEqual(provision._operation(evidence, "fund:sender")["status"], "submitting")


if __name__ == "__main__":
    unittest.main()
