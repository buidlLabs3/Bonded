#!/usr/bin/env python3

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
CLI = REPO / "bin" / "bonded-inbox"


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.data = self.root / "agent"
        self.module = self.root / "module.lgx"
        self.module.write_bytes(b"module")
        self.core = self.root / "logos-core"
        self.core.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        os.chmod(self.core, 0o755)

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, *arguments, expected=0):
        result = subprocess.run(
            [str(CLI), "--data-dir", str(self.data), *arguments],
            cwd=REPO,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, expected, result.stderr)
        return json.loads(result.stdout if expected == 0 else result.stderr)

    def deploy_arguments(self):
        return (
            "--profile", "inbox", "--network", "logos-local",
            "--owner-public-key", "owner-public", "--module", str(self.module),
            "--core-binary", str(self.core), "--test-deployment",
        )

    def test_plan_deploy_status_and_idempotency(self):
        plan = self.run_cli("plan", *self.deploy_arguments())
        self.assertTrue(plan["result"]["ready"])
        deployed = self.run_cli("deploy", *self.deploy_arguments())
        self.assertTrue(deployed["result"]["changed"])
        repeated = self.run_cli("deploy", *self.deploy_arguments())
        self.assertFalse(repeated["result"]["changed"])
        status = self.run_cli("status")
        self.assertEqual(status["result"]["profile"], "inbox")
        self.assertNotIn("isk", json.dumps(status))
        self.assertTrue(self.run_cli("health")["result"]["ready"])

    def test_backup_restore_upgrade_and_rollback(self):
        self.run_cli("deploy", *self.deploy_arguments())
        backup = self.root / "agent.tar.gz"
        self.run_cli("backup", "--output", str(backup))
        new_module = self.root / "module-2.lgx"
        new_module.write_bytes(b"module-2")
        self.run_cli("upgrade", "--module", str(new_module), "--version", "0.2.0")
        self.assertEqual(self.run_cli("status")["result"]["version"], "0.2.0")
        self.run_cli("rollback")
        self.assertEqual(self.run_cli("status")["result"]["version"], "0.1.0")

        restored = self.root / "restored"
        self.data = restored
        self.run_cli("restore", "--input", str(backup))
        self.assertEqual(self.run_cli("status")["result"]["profile"], "inbox")

    def test_teardown_is_guarded(self):
        self.run_cli("deploy", *self.deploy_arguments())
        self.run_cli("teardown", "--confirm", "wrong", expected=2)
        self.assertTrue(self.data.exists())
        self.run_cli("teardown", "--confirm", "DELETE-TEST-DEPLOYMENT")
        self.assertFalse(self.data.exists())


if __name__ == "__main__":
    unittest.main()
