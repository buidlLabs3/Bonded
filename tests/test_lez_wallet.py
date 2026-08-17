#!/usr/bin/env python3

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("lez_wallet", REPO / "tools" / "lez_wallet.py")
wallet = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wallet)


def profile():
    return {
        "schema_version": 1,
        "network": "lez-testnet",
        "lez_release": "v0.2.4",
        "release_commit": wallet.OFFICIAL_RELEASE_COMMIT,
        "source_repository": wallet.OFFICIAL_SOURCE_REPOSITORY,
        "sequencer_url": wallet.OFFICIAL_SEQUENCER,
        "explorer_url": wallet.OFFICIAL_EXPLORER,
        "indexer_url": None,
        "channel_id": "01" * 32,
        "wallet_package": "wallet",
        "wallet_binary_relative": "target/release/wallet",
        "wallet_ffi_header_relative": "lez/wallet-ffi/wallet_ffi.h",
        "wallet_ffi_library_relative": "target/release/deps/libwallet_ffi.so",
        "risc0_dev_mode": 0,
        "required_confirmation_depth": 3,
    }


class OfficialWalletAdapterTests(unittest.TestCase):
    def test_network_profile_rejects_mixed_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            value = profile()
            value["explorer_url"] = "https://example.test"
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(wallet.WalletAdapterError, "explorer_url"):
                wallet.load_network_profile(path)

    def test_inspection_matches_deployment_oracle(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "guest.bin"
            path.write_bytes(b"R0BFbonded")
            result = wallet.inspect_deployment(path)
            self.assertEqual(result["binary_size"], 10)
            self.assertRegex(result["conformance_oracle_transaction_hash"], r"^[0-9a-f]{64}$")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.bin"
            path.write_bytes(b"not-a-program")
            with self.assertRaises(wallet.WalletAdapterError):
                wallet.inspect_deployment(path)

    def test_source_requires_exact_commit_origin_clean_tree_and_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / ".git").mkdir()
            binary = source / "target/release/wallet"
            binary.parent.mkdir(parents=True)
            binary.write_text("wallet", encoding="utf-8")
            binary.chmod(0o700)
            ffi_library = source / "target/release/deps/libwallet_ffi.so"
            ffi_library.parent.mkdir(parents=True)
            ffi_library.write_text("ffi", encoding="utf-8")
            ffi_header = source / "lez/wallet-ffi/wallet_ffi.h"
            ffi_header.parent.mkdir(parents=True)
            ffi_header.write_text("header", encoding="utf-8")
            with mock.patch.object(
                wallet,
                "_run",
                side_effect=[wallet.OFFICIAL_RELEASE_COMMIT, wallet.OFFICIAL_SOURCE_REPOSITORY, ""],
            ):
                result = wallet.verify_source(source, profile())
            self.assertEqual(result["source_commit"], wallet.OFFICIAL_RELEASE_COMMIT)
            self.assertEqual(result["binary_size"], 6)
            self.assertEqual(result["binary_sha256"], wallet.hashlib.sha256(b"wallet").hexdigest())
            self.assertEqual(result["ffi_library_size"], 3)
            self.assertEqual(result["ffi_header_size"], 6)
            with mock.patch.object(wallet, "_run", return_value="ab" * 20):
                with self.assertRaisesRegex(wallet.WalletAdapterError, "expected"):
                    wallet.verify_source(source, profile())

    def test_wallet_home_must_be_external_initialized_private_and_pinned(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            home.chmod(0o700)
            storage = home / "storage.json"
            storage.write_text("encrypted", encoding="utf-8")
            storage.chmod(0o600)
            (home / "wallet_config.json").write_text(
                json.dumps(
                    {
                        "sequencers": [
                            {"sequencer_addr": wallet.OFFICIAL_SEQUENCER, "basic_auth": None}
                        ]
                    }
                ),
                encoding="utf-8",
            )
            self.assertEqual(wallet.verify_wallet_home(home, profile())["home"], str(home.resolve()))
            storage.chmod(0o644)
            with self.assertRaisesRegex(wallet.WalletAdapterError, "group/world"):
                wallet.verify_wallet_home(home, profile())
            storage.chmod(0o600)
            home.chmod(0o755)
            with self.assertRaisesRegex(wallet.WalletAdapterError, "wallet home"):
                wallet.verify_wallet_home(home, profile())
        with self.assertRaisesRegex(wallet.WalletAdapterError, "outside"):
            wallet.verify_wallet_home(REPO, profile())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target"
            target.mkdir()
            link = root / "wallet-link"
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaisesRegex(wallet.WalletAdapterError, "symlink"):
                wallet.verify_wallet_home(link, profile())
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            home.chmod(0o700)
            target = home / "actual-storage"
            target.write_text("encrypted", encoding="utf-8")
            target.chmod(0o600)
            (home / "storage.json").symlink_to(target)
            (home / "wallet_config.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(wallet.WalletAdapterError, "storage and config"):
                wallet.verify_wallet_home(home, profile())

    def test_official_output_requires_exact_hash_and_block(self):
        tx_hash = "ab" * 32
        output = f"Transaction hash is {tx_hash}\nTransaction is included in block 42\n"
        self.assertEqual(
            wallet.parse_official_deployment_output(output, tx_hash),
            {"transaction": tx_hash, "block": 42},
        )
        with self.assertRaisesRegex(wallet.WalletAdapterError, "expected"):
            wallet.parse_official_deployment_output(output, "cd" * 32)
        with self.assertRaisesRegex(wallet.WalletAdapterError, "recovery/key"):
            wallet.parse_official_deployment_output(
                "Recovery phrase:\nsecret words\n" + output, tx_hash
            )

    def test_submission_requires_two_explicit_authorizations(self):
        args = mock.Mock(submit=False)
        with mock.patch.dict(os.environ, {"BONDED_LEZ_SUBMIT": "YES"}, clear=False):
            with self.assertRaisesRegex(wallet.WalletAdapterError, "both"):
                wallet.deploy_program(args, profile())

    def test_atomic_wallet_journal_is_private(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "journal.json"
            value = {"status": "verified"}
            wallet.atomic_json(path, value)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), value)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_release_script_delegates_to_official_wallet(self):
        script = (REPO / "scripts" / "deploy-lez-testnet.sh").read_text(encoding="utf-8")
        self.assertIn("deploy-lez-official-wallet.sh", script)
        self.assertNotIn("tools/lez_testnet.py", script)
        self.assertNotIn("sendTransaction", script)


if __name__ == "__main__":
    unittest.main()
