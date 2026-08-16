#!/usr/bin/env python3

import io
import json
import os
import subprocess
import tarfile
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
CLI = REPO / "bin" / "bonded-inbox"
COMMIT = "a" * 40


FAKE_CORE = r'''#!/usr/bin/env python3
import json
import os
import signal
import sys
import time
from pathlib import Path

args = sys.argv[1:]
config = Path(args[args.index("--config-dir") + 1])
daemon_state = config / "daemon" / "state.json"

def output(value):
    print(json.dumps(value))

if "-D" in args:
    daemon_state.parent.mkdir(parents=True, exist_ok=True)
    daemon_state.write_text(json.dumps({"pid": os.getpid()}))
    def stop(*_):
        daemon_state.unlink(missing_ok=True)
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    while True:
        time.sleep(0.1)

command = next((item for item in ("status", "load-module", "call", "stop") if item in args), "")
if command == "status":
    if not daemon_state.exists():
        output({"daemon": {"status": "not_running"}})
        raise SystemExit(1)
    pid = json.loads(daemon_state.read_text())["pid"]
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        output({"daemon": {"status": "not_running"}})
        raise SystemExit(1)
    output({"daemon": {"status": "running", "pid": pid}})
elif command == "load-module":
    output({"status": "ok", "module": "bonded_inbox", "version": "0.1.0",
            "dependencies_loaded": ["delivery_module", "storage_module", "lez_core"]})
elif command == "call":
    index = args.index("call")
    module, method = args[index + 1:index + 3]
    values = [item for item in args[index + 3:] if item != "--json"]
    if module == "delivery_module":
        result = {"success": True, "value": None, "error": None}
    elif module == "storage_module":
        result = True
    elif module == "lez_core" and method == "open":
        result = 0
    elif module == "bonded_inbox" and method == "initialize":
        configuration = json.loads(Path(values[0][1:]).read_text())
        (config / "runtime.json").write_text(json.dumps(configuration))
        result = json.dumps({"ok": True, "result": {"profile": configuration["profile"]}})
    elif module == "bonded_inbox" and method == "getStatus":
        configuration = json.loads((config / "runtime.json").read_text())
        module_status = {"state": "ready", "profile": configuration["profile"],
                         "runtime": {"state": "ready", "agent_id": "npk:test-agent"}}
        result = json.dumps({"ok": True, "result": module_status})
    elif module == "bonded_inbox" and method == "invokeSkill":
        request = json.loads(Path(values[0][1:]).read_text())
        result = json.dumps({"ok": True, "result": {"skill": request["skill"], "live": True}})
    else:
        output({"status": "error", "message": "unexpected fake call"})
        raise SystemExit(4)
    output({"status": "ok", "module": module, "method": method, "result": result})
elif command == "stop":
    if daemon_state.exists():
        pid = json.loads(daemon_state.read_text())["pid"]
        os.kill(pid, signal.SIGTERM)
        for _ in range(100):
            if not daemon_state.exists():
                break
            time.sleep(0.01)
    output({"status": "ok"})
else:
    output({"status": "error", "message": "unexpected fake command"})
    raise SystemExit(1)
'''


class CliTests(unittest.TestCase):
    @staticmethod
    def write_lgx(path, name):
        manifest = json.dumps({"name": name, "version": "test", "type": "core"}).encode()
        with tarfile.open(path, "w:gz") as archive:
            info = tarfile.TarInfo("manifest.json")
            info.size = len(manifest)
            archive.addfile(info, io.BytesIO(manifest))

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.data = self.root / "agent"
        self.module = self.root / "bonded.lgx"
        self.write_lgx(self.module, "bonded_inbox")
        self.dependencies = []
        for name in ("delivery_module", "storage_module", "lez_core"):
            package = self.root / f"{name}.lgx"
            self.write_lgx(package, name)
            self.dependencies.append(package)
        self.core = self.root / "logoscore"
        self.core.write_text(textwrap.dedent(FAKE_CORE), encoding="utf-8")
        os.chmod(self.core, 0o755)
        self.package_manager = self.root / "lgpm"
        self.package_manager.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        os.chmod(self.package_manager, 0o755)

    def tearDown(self):
        if (self.data / "deployment.json").exists():
            subprocess.run(
                [str(CLI), "--data-dir", str(self.data), "stop"],
                cwd=REPO, capture_output=True, check=False,
            )
        self.temporary.cleanup()

    def run_cli(self, *arguments, expected=0):
        result = subprocess.run(
            [str(CLI), "--data-dir", str(self.data), *arguments],
            cwd=REPO, text=True, capture_output=True, check=False, timeout=30,
        )
        self.assertEqual(result.returncode, expected, result.stderr)
        return json.loads(result.stdout if expected == 0 else result.stderr)

    def deploy_arguments(self, profile="inbox"):
        arguments = [
            "--profile", profile, "--network", "lez-testnet",
            "--owner-public-key", "b" * 64, "--source-commit", COMMIT,
            "--module", str(self.module), "--core-binary", str(self.core),
            "--package-manager", str(self.package_manager), "--start-timeout", "5",
            "--test-deployment",
        ]
        for dependency in self.dependencies:
            arguments.extend(("--dependency-module", str(dependency)))
        return tuple(arguments)

    def test_plan_deploy_health_and_idempotency(self):
        plan = self.run_cli("plan", *self.deploy_arguments())
        self.assertTrue(plan["result"]["ready"])
        deployed = self.run_cli("deploy", *self.deploy_arguments())
        self.assertTrue(deployed["result"]["changed"])
        self.assertEqual(deployed["result"]["deployment"]["agent_id"], "npk:test-agent")
        repeated = self.run_cli("deploy", *self.deploy_arguments())
        self.assertFalse(repeated["result"]["changed"])
        status = self.run_cli("status")["result"]
        self.assertTrue(status["core_running"])
        self.assertEqual(status["module"]["state"], "ready")
        self.assertTrue(self.run_cli("health")["result"]["ready"])
        evidence = json.loads((self.data / "evidence" / "deployment.json").read_text())
        self.assertEqual(evidence["source_commit"], COMMIT)
        self.assertEqual(set(evidence["artifacts"]), {
            "bonded_inbox", "delivery_module", "storage_module", "lez_core",
        })
        self.assertNotIn("owner_public_key", evidence)

    def test_plan_rejects_invalid_owner_key_and_limits(self):
        arguments = list(self.deploy_arguments())
        arguments[arguments.index("--owner-public-key") + 1] = "not-a-key"
        arguments.extend(("--per-period-limit", "0"))
        plan = self.run_cli("plan", *arguments)["result"]
        self.assertFalse(plan["ready"])
        self.assertFalse(plan["checks"]["owner_key_valid"])
        self.assertFalse(plan["checks"]["limits_positive"])

    def test_plan_rejects_wrong_dependency_package(self):
        self.write_lgx(self.dependencies[-1], "not_lez_core")
        plan = self.run_cli("plan", *self.deploy_arguments())["result"]
        self.assertFalse(plan["ready"])
        self.assertFalse(plan["checks"]["dependency_package_names_valid"])

    def test_stop_restart_and_live_skill_invocation(self):
        self.run_cli("deploy", *self.deploy_arguments())
        stopped = self.run_cli("stop")["result"]
        self.assertTrue(stopped["changed"])
        self.assertFalse(self.run_cli("health")["result"]["ready"])
        restarted = self.run_cli("start")["result"]
        self.assertTrue(restarted["changed"])
        payload = self.root / "input.json"
        payload.write_text("{}\n", encoding="utf-8")
        invoked = self.run_cli("invoke", "--skill", "meta.status", "--input", str(payload))
        self.assertEqual(invoked["result"], {"skill": "meta.status", "live": True})

    def test_settlement_requires_real_wallet_inputs(self):
        failed = self.run_cli("plan", *self.deploy_arguments("settlement"))
        self.assertFalse(failed["result"]["ready"])
        self.assertFalse(failed["result"]["checks"]["lez_wallet_files_present"])
        self.assertFalse(failed["result"]["checks"]["lez_account_id_valid"])

    def test_teardown_is_guarded_and_stops_daemon(self):
        self.run_cli("deploy", *self.deploy_arguments())
        self.run_cli("teardown", "--confirm", "wrong", expected=2)
        self.assertTrue(self.data.exists())
        self.run_cli("teardown", "--confirm", "DELETE-TEST-DEPLOYMENT")
        self.assertFalse(self.data.exists())


if __name__ == "__main__":
    unittest.main()
