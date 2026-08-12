#!/usr/bin/env python3

import importlib.util
import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_wallet_bootstrap", REPO / "tools" / "lez_wallet_bootstrap.py"
)
bootstrap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bootstrap)


class WalletBootstrapTests(unittest.TestCase):
    def test_target_must_be_absolute_external_and_new(self):
        with self.assertRaisesRegex(bootstrap.BootstrapError, "absolute"):
            bootstrap.validate_target(Path("relative-wallet"))
        with self.assertRaisesRegex(bootstrap.BootstrapError, "outside"):
            bootstrap.validate_target(REPO / "private-wallet")
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(bootstrap.BootstrapError, "already exists"):
                bootstrap.validate_target(Path(directory))
            target = Path(directory) / "new-wallet"
            self.assertEqual(bootstrap.validate_target(target), target)

    def test_private_writer_is_exclusive_and_mode_0600(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "secret"
            bootstrap.write_private(path, b"private\n")
            self.assertEqual(path.read_bytes(), b"private\n")
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            with self.assertRaises(FileExistsError):
                bootstrap.write_private(path, b"overwrite\n")

    def test_creation_requires_both_explicit_gates(self):
        args = type("Args", (), {"create": False})()
        with self.assertRaisesRegex(bootstrap.BootstrapError, "requires"):
            bootstrap.create_wallet(args)
        args.create = True
        previous = os.environ.pop("BONDED_LEZ_BOOTSTRAP", None)
        try:
            with self.assertRaisesRegex(bootstrap.BootstrapError, "requires"):
                bootstrap.create_wallet(args)
        finally:
            if previous is not None:
                os.environ["BONDED_LEZ_BOOTSTRAP"] = previous

    def test_failed_creation_removes_only_its_staging_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "wallet"
            args = type(
                "Args",
                (),
                {
                    "create": True,
                    "wallet_home": target,
                    "profile": REPO / "config/lez-testnet-network.json",
                    "wallet_source": Path("/not-used"),
                },
            )()
            profile = bootstrap.lez_wallet.load_network_profile(args.profile)
            provenance = {
                "ffi_library": "/not-a-library",
                "source_commit": profile["release_commit"],
                "ffi_library_sha256": "00" * 32,
            }
            with mock.patch.dict(os.environ, {"BONDED_LEZ_BOOTSTRAP": "YES"}):
                with mock.patch.object(
                    bootstrap.lez_wallet, "verify_source", return_value=provenance
                ):
                    with self.assertRaises(OSError):
                        bootstrap.create_wallet(args)
            self.assertFalse(target.exists())
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
