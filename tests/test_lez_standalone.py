#!/usr/bin/env python3

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_standalone", REPO / "tools" / "lez_standalone.py"
)
standalone = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(standalone)


class StandaloneQualificationTests(unittest.TestCase):
    def test_commands_are_exact_locked_release_single_test_runs(self):
        for test in standalone.TESTS:
            command = standalone._command(test)
            self.assertEqual(command[:4], ["cargo", "test", "--locked", "--release"])
            self.assertIn("--exact", command)
            self.assertIn("--test-threads=1", command)

    def test_log_scan_extracts_public_hashes_without_retaining_log(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log"
            tx_hash = "ab" * 32
            path.write_text(
                f"Transaction hash is {tx_hash}\n"
                "Transaction is included in block 42\n"
                "Transaction data is PrivacyPreserving(test-vector)\n"
                "test result: ok. 1 passed; 0 failed;\n",
                encoding="utf-8",
            )
            result = standalone._scan_log(path, standalone.TESTS[0], 1.25)
            self.assertEqual(result["transaction_hashes"], [tx_hash])
            self.assertEqual(result["block_ids"], [42])
            self.assertEqual(len(result["official_transaction_debug_sha256"]), 1)
            self.assertNotIn("captured.log", str(result))

    def test_secret_marker_and_missing_transaction_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "log"
            path.write_text(
                "Recovery phrase: do not persist\ntest result: ok. 1 passed;\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(standalone.StandaloneQualificationError, "secret"):
                standalone._scan_log(path, standalone.TESTS[0], 1.0)
            path.write_text("test result: ok. 1 passed;\n", encoding="utf-8")
            with self.assertRaisesRegex(standalone.StandaloneQualificationError, "hash"):
                standalone._scan_log(path, standalone.TESTS[0], 1.0)

    def test_resume_rejects_different_wallet_and_accepts_matching_partial(self):
        profile = {
            "lez_release": "v0.2.4",
            "release_commit": "release",
            "source_repository": "official",
        }
        provenance = {
            "source_commit": "release",
            "binary_sha256": "ab" * 32,
            "binary_size": 123,
        }
        result = {"id": standalone.TESTS[0]["id"], "status": "passed"}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            artifact = standalone._artifact(profile, provenance, [result])
            path.write_text(json.dumps(artifact), encoding="utf-8")
            self.assertEqual(
                standalone._resume_results(path, profile, provenance), [result]
            )
            provenance["binary_size"] += 1
            with self.assertRaisesRegex(
                standalone.StandaloneQualificationError, "does not match"
            ):
                standalone._resume_results(path, profile, provenance)

    def test_partial_and_complete_statuses_are_explicit(self):
        profile = {
            "lez_release": "v0.2.4",
            "release_commit": "release",
            "source_repository": "official",
        }
        provenance = {
            "source_commit": "release",
            "binary_sha256": "ab" * 32,
            "binary_size": 123,
        }
        partial = standalone._artifact(
            profile, provenance, [{"id": standalone.TESTS[0]["id"]}]
        )
        self.assertEqual(partial["status"], "qualification-in-progress")
        complete = standalone._artifact(
            profile,
            provenance,
            [{"id": test["id"]} for test in standalone.TESTS],
        )
        self.assertEqual(complete["status"], "verified-standalone-real-proof")

    def test_qualification_requires_two_gates_and_real_proof_mode(self):
        args = mock.Mock(run=False)
        with mock.patch.dict(os.environ, {"BONDED_LEZ_STANDALONE": "YES"}, clear=False):
            with self.assertRaisesRegex(standalone.StandaloneQualificationError, "both"):
                standalone.qualify(args)
        args.run = True
        with mock.patch.dict(
            os.environ,
            {"BONDED_LEZ_STANDALONE": "YES", "RISC0_DEV_MODE": "1"},
            clear=False,
        ):
            with self.assertRaisesRegex(standalone.StandaloneQualificationError, "exactly 0"):
                standalone.qualify(args)


if __name__ == "__main__":
    unittest.main()
