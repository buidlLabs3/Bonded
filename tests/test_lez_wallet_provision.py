#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


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
