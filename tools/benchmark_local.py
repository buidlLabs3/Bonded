#!/usr/bin/env python3

"""Reproducible, fail-closed qualification for supported local adapter limits."""

import argparse
import json
import os
import platform
import shlex
import statistics
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
CLI = REPO / "bin" / "bonded-inbox"
NATIVE_SOURCE = REPO / "benchmarks" / "local_workloads.cpp"
LIMITS = {
    "cli_deploy_p95_ms": {"comparison": "maximum", "threshold": 1500.0, "unit": "ms"},
    "cli_status_p95_ms": {"comparison": "maximum", "threshold": 1500.0, "unit": "ms"},
    "queue_operations_per_second": {
        "comparison": "minimum",
        "threshold": 20000.0,
        "unit": "operations/second",
    },
    "spam_attempts_per_second": {
        "comparison": "minimum",
        "threshold": 20000.0,
        "unit": "attempts/second",
    },
    "attachment_mib_per_second": {
        "comparison": "minimum",
        "threshold": 5.0,
        "unit": "MiB/second",
    },
    "a2a_tasks_per_second": {
        "comparison": "minimum",
        "threshold": 50.0,
        "unit": "tasks/second",
    },
    "recovery_reopen_ms": {"comparison": "maximum", "threshold": 500.0, "unit": "ms"},
    "settlement_operations_per_second": {
        "comparison": "minimum",
        "threshold": 5000.0,
        "unit": "operations/second",
    },
    "peak_rss_kib": {"comparison": "maximum", "threshold": 262144.0, "unit": "KiB"},
}


class BenchmarkError(RuntimeError):
    pass


def latency(command, repetitions=1):
    samples = []
    for _ in range(repetitions):
        started = time.perf_counter()
        subprocess.run(command, cwd=REPO, check=True, capture_output=True, text=True)
        samples.append((time.perf_counter() - started) * 1000)
    ordered = sorted(samples)
    p95_index = max(0, int(len(ordered) * 0.95 + 0.999999) - 1)
    return {
        "minimum_ms": min(samples),
        "maximum_ms": max(samples),
        "mean_ms": statistics.fmean(samples),
        "p95_ms": ordered[p95_index],
        "samples": len(samples),
    }


def compile_native(output: Path) -> None:
    try:
        environment = os.environ.copy()
        environment.pop("NIX_LDFLAGS", None)
        environment.pop("LDFLAGS", None)
        flags = subprocess.run(
            ["pkg-config", "--cflags", "--libs", "openssl", "sqlite3", "nlohmann_json"],
            cwd=REPO,
            check=True,
            capture_output=True,
            text=True,
            env=environment,
        ).stdout
        command = [
            "g++",
            "-std=c++20",
            "-O2",
            "-DNDEBUG",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-Isrc",
            str(NATIVE_SOURCE),
            "src/domain/state_machine.cpp",
            "src/security/crypto.cpp",
            "src/storage/database.cpp",
            "src/integrations/memory_adapters.cpp",
            "src/runtime/reliability.cpp",
            "src/services/contact_rules.cpp",
            "src/services/a2a_service.cpp",
            "src/services/bond_service.cpp",
            *shlex.split(flags),
            "-pthread",
            "-o",
            str(output),
        ]
        subprocess.run(
            command,
            cwd=REPO,
            check=True,
            capture_output=True,
            text=True,
            env=environment,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = exc.stderr.strip() if isinstance(exc, subprocess.CalledProcessError) else str(exc)
        raise BenchmarkError(f"could not build native benchmark: {detail}") from exc


def run_native(binary: Path, database: Path) -> dict:
    try:
        process = subprocess.run(
            [str(binary), str(database)],
            cwd=REPO,
            check=True,
            capture_output=True,
            text=True,
            timeout=300,
        )
        report = json.loads(process.stdout)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, json.JSONDecodeError) as exc:
        detail = exc.stderr.strip() if isinstance(exc, subprocess.CalledProcessError) else str(exc)
        raise BenchmarkError(f"native benchmark failed: {detail}") from exc
    if not isinstance(report, dict) or report.get("schema_version") != 1:
        raise BenchmarkError("native benchmark returned an unsupported report")
    return report


def metric_values(cli_deploy: dict, cli_status: dict, native: dict) -> dict[str, float]:
    return {
        "cli_deploy_p95_ms": float(cli_deploy["p95_ms"]),
        "cli_status_p95_ms": float(cli_status["p95_ms"]),
        "queue_operations_per_second": float(native["queue"]["operations_per_second"]),
        "spam_attempts_per_second": float(native["spam_burst"]["attempts_per_second"]),
        "attachment_mib_per_second": float(native["attachment_round_trip"]["mib_per_second"]),
        "a2a_tasks_per_second": float(native["a2a_concurrency"]["tasks_per_second"]),
        "recovery_reopen_ms": float(native["reconnect_recovery"]["reopen_and_verify_ms"]),
        "settlement_operations_per_second": float(
            native["settlement"]["lock_and_settle_operations_per_second"]
        ),
        "peak_rss_kib": float(native["peak_rss_kib"]),
    }


def evaluate(values: dict[str, float]) -> dict[str, bool]:
    if set(values) != set(LIMITS):
        raise BenchmarkError("benchmark metrics do not match the supported limit set")
    return {
        name: (
            value >= rule["threshold"]
            if rule["comparison"] == "minimum"
            else value <= rule["threshold"]
        )
        for name, value in values.items()
        for rule in (LIMITS[name],)
    }


def source_commit() -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BenchmarkError(f"could not resolve source commit: {exc}") from exc


def qualify(native_binary: Path | None = None) -> dict:
    with tempfile.TemporaryDirectory(prefix="bonded-local-benchmark-") as temporary:
        root = Path(temporary)
        module = root / "module.lgx"
        module.write_bytes(b"benchmark-module")
        core = root / "logos-core"
        core.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        core.chmod(0o755)
        data = root / "agent"
        base = [str(CLI), "--data-dir", str(data)]
        deploy = base + [
            "deploy",
            "--profile",
            "inbox",
            "--network",
            "logos-local",
            "--owner-public-key",
            "benchmark-owner",
            "--module",
            str(module),
            "--core-binary",
            str(core),
            "--test-deployment",
        ]
        binary = native_binary or root / "local-workloads"
        if native_binary is None:
            compile_native(binary)
        native = run_native(binary.resolve(strict=True), root / "recovery.db")
        deploy_latency = latency(deploy)
        status_latency = latency(base + ["status"], repetitions=25)
        values = metric_values(deploy_latency, status_latency, native)
        checks = evaluate(values)
        return {
            "schema_version": 1,
            "status": "pass" if all(checks.values()) else "fail",
            "scope": "local-adapters-only-not-lez-or-cu-evidence",
            "observed_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
            "source_commit": source_commit(),
            "environment": {
                "system": platform.platform(),
                "machine": platform.machine(),
                "python": platform.python_version(),
            },
            "limits": LIMITS,
            "metric_values": values,
            "checks": checks,
            "measurements": {
                "cli_deploy": deploy_latency,
                "cli_status": status_latency,
                "native": native,
            },
            "unsupported_claims": {
                "attachment_streaming": "adapter accepts a complete in-memory object",
                "logos_multi_process": "official Messaging/Storage/Core adapters are not bound",
                "lez_compute_units": "requires finalized official testnet receipts",
                "real_proof_latency": "standalone real-proof qualification is separate",
            },
            "cu_measured": False,
        }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Qualify supported local Bonded limits")
    result.add_argument("--native-binary", type=Path)
    result.add_argument("--output", type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        report = qualify(args.native_binary)
        serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(serialized)
        print(serialized, end="")
        return 0 if report["status"] == "pass" else 3
    except (BenchmarkError, FileExistsError, OSError, ValueError, KeyError) as exc:
        print(str(exc), file=__import__("sys").stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
