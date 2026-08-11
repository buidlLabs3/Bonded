#!/usr/bin/env python3

import json
import platform
import subprocess
import tempfile
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
CLI = REPO / "bin" / "bonded-inbox"


def timed(command, repetitions=1):
    samples = []
    for _ in range(repetitions):
        started = time.perf_counter()
        subprocess.run(command, cwd=REPO, check=True, capture_output=True, text=True)
        samples.append((time.perf_counter() - started) * 1000)
    return {
        "minimum_ms": min(samples),
        "maximum_ms": max(samples),
        "mean_ms": sum(samples) / len(samples),
        "samples": len(samples),
    }


def main():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        module = root / "module.lgx"
        module.write_bytes(b"benchmark-module")
        core = root / "logos-core"
        core.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        core.chmod(0o755)
        data = root / "agent"
        base = [str(CLI), "--data-dir", str(data)]
        deploy = base + [
            "deploy", "--profile", "inbox", "--network", "logos-local",
            "--owner-public-key", "benchmark-owner", "--module", str(module),
            "--core-binary", str(core), "--test-deployment",
        ]
        report = {
            "mode": "local-adapters",
            "system": platform.platform(),
            "python": platform.python_version(),
            "deploy": timed(deploy),
            "status": timed(base + ["status"], repetitions=25),
            "cu_measured": False,
        }
        print(json.dumps(report, sort_keys=True, indent=2))


if __name__ == "__main__":
    main()
