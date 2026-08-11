#!/usr/bin/env python3

import json
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


class AssetTests(unittest.TestCase):
    def test_basecamp_manifest_and_states(self):
        manifest = json.loads((REPO / "basecamp" / "manifest.json").read_text(encoding="utf-8"))
        entry = REPO / "basecamp" / manifest["entrypoint"]
        self.assertTrue(entry.is_file())
        qml = entry.read_text(encoding="utf-8")
        for marker in (
            "pendingMessageList", "taskList", "approvalList", "rejectionConfirmation",
            "offline", "loading", "Accessible.name", "rejectionSink",
        ):
            self.assertIn(marker, qml)
        self.assertNotIn("private_key", qml)
        self.assertNotIn("seed", qml.lower())

    def test_profiles_are_distinct(self):
        profiles = []
        for name in ("inbox", "vault", "settlement"):
            value = json.loads((REPO / "profiles" / f"{name}.json").read_text(encoding="utf-8"))
            profiles.append(value["profile"])
        self.assertEqual(len(set(profiles)), 3)


if __name__ == "__main__":
    unittest.main()
