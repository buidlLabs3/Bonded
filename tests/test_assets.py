#!/usr/bin/env python3

import json
import os
import subprocess
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
            "offline", "loading", "Accessible.name", "rejectionSink", "vaultPlaintext",
            "vaultAddress", "observedAccount", "skill.invoke", "toolResult",
        ):
            self.assertIn(marker, qml)
        self.assertNotIn("private_key", qml)
        self.assertNotIn("seed", qml.lower())

    def test_basecamp_preview_is_interactive_and_secret_free(self):
        preview = (REPO / "basecamp" / "preview" / "Main.qml").read_text(encoding="utf-8")
        for marker in (
            "BondedInboxPage", "pendingMessages", "activeTasks", "approvals",
            "onRefreshRequested", "onDecisionRequested", "onApprovalRequested",
            "onConfigurationRequested", "minimumWidth", "minimumHeight",
        ):
            self.assertIn(marker, preview)
        self.assertNotIn("private_key", preview)
        self.assertNotIn("seed", preview.lower())

    @unittest.skipUnless(os.environ.get("BONDED_QML_RUNNER"), "BONDED_QML_RUNNER is not set")
    def test_basecamp_qml_instantiates_offscreen(self):
        environment = os.environ.copy()
        environment["QT_QPA_PLATFORM"] = "offscreen"
        completed = subprocess.run(
            [str(REPO / "scripts" / "run-basecamp-preview.sh"), "--smoke"],
            cwd=REPO,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_profiles_are_distinct(self):
        profiles = []
        for name in ("inbox", "vault", "settlement"):
            value = json.loads((REPO / "profiles" / f"{name}.json").read_text(encoding="utf-8"))
            profiles.append(value["profile"])
        self.assertEqual(len(set(profiles)), 3)


if __name__ == "__main__":
    unittest.main()
