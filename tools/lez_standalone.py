#!/usr/bin/env python3

"""Run sanitized, real-proof qualification against the pinned LEZ standalone stack."""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
WALLET_SPEC = importlib.util.spec_from_file_location(
    "lez_wallet", REPO_ROOT / "tools" / "lez_wallet.py"
)
lez_wallet = importlib.util.module_from_spec(WALLET_SPEC)
WALLET_SPEC.loader.exec_module(lez_wallet)

PASS_MARKER = re.compile(r"test result: ok\. 1 passed;")
TX_HASH = re.compile(r"(?i)Transaction hash is ([0-9a-f]{64})")
BLOCK_ID = re.compile(r"Transaction is included in block ([0-9]+)")
TRANSACTION_DATA = re.compile(r"(?m)^Transaction data is (.+)$")
COMPOSE_PROJECT = re.compile(r"^[0-9a-f-]{36}$")
SECRET_MARKERS = lez_wallet.SENSITIVE_MARKERS + (
    "generated new private",
    "nullifier secret",
    "viewing secret",
    "secret key:",
)
TESTS = (
    {
        "id": "official-wallet-program-deployment",
        "package": "integration_tests",
        "target": "program_deployment",
        "filter": "deploy_and_execute_program",
        "proof_mode": "unsigned-program-deployment",
        "expected_transactions_min": 1,
    },
    {
        "id": "official-wallet-private-transfer-real-proof",
        "package": "integration_tests",
        "target": "auth_transfer",
        "filter": "private::private_transfer_to_owned_account",
        "proof_mode": "risc0-real-proof",
        "expected_transactions_min": 1,
    },
    {
        "id": "invalid-program-rejected",
        "package": "integration_tests",
        "target": "program_deployment",
        "filter": "deploy_invalid_program_fails",
        "proof_mode": "negative-validation",
        "expected_transactions_min": 0,
    },
    {
        "id": "insufficient-funds-rejected",
        "package": "integration_tests",
        "target": "auth_transfer",
        "filter": "public::failed_transfer_with_insufficient_balance",
        "proof_mode": "negative-validation",
        "expected_transactions_min": 0,
    },
    {
        "id": "duplicate-transaction-rejected",
        "package": "sequencer_core",
        "target": None,
        "cargo_args": ("--features", "mock", "--lib"),
        "filter": "tests::replay_transactions_are_rejected_in_different_blocks",
        "proof_mode": "negative-validation",
        "expected_transactions_min": 0,
    },
)
TEST_BY_ID = {test["id"]: test for test in TESTS}


class StandaloneQualificationError(RuntimeError):
    pass


def _command(test: dict) -> list[str]:
    command = [
        "cargo",
        "test",
        "--locked",
        "--release",
        "-p",
        test["package"],
    ]
    if test["target"]:
        command.extend(["--test", test["target"]])
    command.extend(test.get("cargo_args", ()))
    command.extend([test["filter"], "--", "--exact", "--nocapture", "--test-threads=1"])
    return command


def _scan_log(path: Path, test: dict, elapsed_seconds: float) -> dict:
    data = path.read_bytes()
    text = data.decode("utf-8", errors="replace")
    lowered = text.lower()
    if any(marker in lowered for marker in SECRET_MARKERS):
        raise StandaloneQualificationError(
            f"{test['id']} emitted potential secret material; log was suppressed"
        )
    if not PASS_MARKER.search(text):
        raise StandaloneQualificationError(
            f"{test['id']} did not report exactly one passing test; log was suppressed"
        )
    transactions = [value.lower() for value in TX_HASH.findall(text)]
    blocks = [int(value) for value in BLOCK_ID.findall(text)]
    transaction_data = TRANSACTION_DATA.findall(text)
    if len(transactions) < test["expected_transactions_min"]:
        raise StandaloneQualificationError(
            f"{test['id']} omitted the expected wallet transaction hash"
        )
    if test["expected_transactions_min"] and len(blocks) < test["expected_transactions_min"]:
        raise StandaloneQualificationError(
            f"{test['id']} omitted the expected inclusion block"
        )
    if test["expected_transactions_min"] and len(transaction_data) < test["expected_transactions_min"]:
        raise StandaloneQualificationError(
            f"{test['id']} omitted the official wallet transaction representation"
        )
    return {
        "id": test["id"],
        "status": "passed",
        "proof_mode": test["proof_mode"],
        "upstream_test": test["filter"],
        "transaction_hashes": transactions,
        "block_ids": blocks,
        "official_transaction_debug_sha256": [
            hashlib.sha256(value.encode("utf-8")).hexdigest()
            for value in transaction_data
        ],
        "captured_log_sha256": hashlib.sha256(data).hexdigest(),
        "captured_log_bytes": len(data),
        "elapsed_seconds": round(elapsed_seconds, 3),
    }


def _owned_compose_projects(source: Path) -> set[str]:
    """Return only Compose projects using this pinned checkout's Bedrock file."""
    expected = (source / "bedrock" / "docker-compose.yml").resolve()
    try:
        listed = subprocess.run(
            [
                "docker",
                "ps",
                "-aq",
                "--filter",
                "label=com.docker.compose.project",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
        identifiers = listed.stdout.split()
        if not identifiers:
            return set()
        inspected = subprocess.run(
            ["docker", "inspect", *identifiers],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
        projects = set()
        for container in json.loads(inspected.stdout):
            labels = container.get("Config", {}).get("Labels") or {}
            project = labels.get("com.docker.compose.project", "")
            config_files = labels.get("com.docker.compose.project.config_files", "")
            paths = {
                Path(value).resolve()
                for value in config_files.split(",")
                if value.strip()
            }
            if COMPOSE_PROJECT.fullmatch(project) and expected in paths:
                projects.add(project)
        return projects
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        return set()


def _cleanup_compose_projects(source: Path, projects: set[str]) -> None:
    compose_file = (source / "bedrock" / "docker-compose.yml").resolve()
    for project in sorted(projects):
        if not COMPOSE_PROJECT.fullmatch(project):
            continue
        subprocess.run(
            [
                "docker",
                "compose",
                "-p",
                project,
                "-f",
                str(compose_file),
                "down",
                "--remove-orphans",
            ],
            cwd=compose_file.parent,
            capture_output=True,
            timeout=120,
            check=False,
        )


def _terminate_process_group(process: subprocess.Popen) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=15)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_test(source: Path, test: dict, timeout: float, execution: dict) -> dict:
    environment = os.environ.copy()
    environment.update(
        {
            "RISC0_DEV_MODE": "0",
            "RISC0_PROVER": execution["risc0_prover"],
            "RAYON_NUM_THREADS": str(execution["rayon_num_threads"]),
            "RUST_LOG": "warn",
            "CARGO_TERM_COLOR": "never",
        }
    )
    with tempfile.TemporaryDirectory(prefix="bonded-lez-real-proof-") as directory:
        temp = Path(directory)
        temp.chmod(0o700)
        log_path = temp / "captured.log"
        started = time.monotonic()
        projects_before = _owned_compose_projects(source)
        try:
            with log_path.open("wb") as output:
                log_path.chmod(0o600)
                process = subprocess.Popen(
                    _command(test),
                    cwd=source,
                    env=environment,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                try:
                    returncode = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired as exc:
                    _terminate_process_group(process)
                    created = _owned_compose_projects(source) - projects_before
                    _cleanup_compose_projects(source, created)
                    raise StandaloneQualificationError(
                        f"{test['id']} exceeded its {timeout:g}-second qualification budget; "
                        "this is an incomplete run, not a protocol rejection"
                    ) from exc
        except OSError as exc:
            raise StandaloneQualificationError(
                f"{test['id']} could not complete: {exc}"
            ) from exc
        elapsed = time.monotonic() - started
        if returncode != 0:
            created = _owned_compose_projects(source) - projects_before
            _cleanup_compose_projects(source, created)
            raise StandaloneQualificationError(
                f"{test['id']} failed with exit {returncode}; log was suppressed"
            )
        return _scan_log(log_path, test, elapsed)


def _execution_profile(args) -> dict:
    if args.rayon_threads <= 0:
        raise StandaloneQualificationError("Rayon thread count must be positive")
    return {
        "risc0_prover": args.prover,
        "rayon_num_threads": args.rayon_threads,
    }


def _artifact(
    profile: dict, provenance: dict, results: list[dict], execution: dict
) -> dict:
    completed = {result["id"] for result in results}
    required = set(TEST_BY_ID)
    return {
        "schema_version": 1,
        "status": (
            "verified-standalone-real-proof"
            if completed == required
            else "qualification-in-progress"
        ),
        "scope": "local-standalone-not-public-testnet-evidence",
        "risc0_dev_mode": 0,
        "execution": execution,
        "network_identity": {
            "lez_release": profile["lez_release"],
            "lez_release_commit": profile["release_commit"],
            "standalone_channel": "isolated-per-test",
        },
        "official_wallet": {
            "source_repository": profile["source_repository"],
            "source_commit": provenance["source_commit"],
            "binary_sha256": provenance["binary_sha256"],
            "binary_size": provenance["binary_size"],
        },
        "completed_case_ids": sorted(completed),
        "required_case_ids": sorted(required),
        "tests": sorted(results, key=lambda result: result["id"]),
        "observed_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
    }


def _resume_results(
    path: Path, profile: dict, provenance: dict, execution: dict
) -> list[dict]:
    if not path.exists():
        return []
    try:
        prior = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise StandaloneQualificationError(f"could not resume evidence: {exc}") from exc
    expected_identity = {
        "lez_release": profile["lez_release"],
        "lez_release_commit": profile["release_commit"],
        "standalone_channel": "isolated-per-test",
    }
    expected_wallet = {
        "source_repository": profile["source_repository"],
        "source_commit": provenance["source_commit"],
        "binary_sha256": provenance["binary_sha256"],
        "binary_size": provenance["binary_size"],
    }
    if (
        prior.get("schema_version") != 1
        or prior.get("scope") != "local-standalone-not-public-testnet-evidence"
        or prior.get("risc0_dev_mode") != 0
        or prior.get("execution") != execution
        or prior.get("network_identity") != expected_identity
        or prior.get("official_wallet") != expected_wallet
    ):
        raise StandaloneQualificationError(
            "existing standalone evidence does not match the pinned qualification inputs"
        )
    results = prior.get("tests")
    if not isinstance(results, list):
        raise StandaloneQualificationError("existing standalone evidence has invalid tests")
    ids = [result.get("id") for result in results if isinstance(result, dict)]
    if len(ids) != len(results) or len(ids) != len(set(ids)) or not set(ids) <= set(TEST_BY_ID):
        raise StandaloneQualificationError("existing standalone evidence has invalid case IDs")
    return results


def qualify(args) -> dict:
    if not args.run or os.environ.get("BONDED_LEZ_STANDALONE") != "YES":
        raise StandaloneQualificationError(
            "qualification requires both --run and BONDED_LEZ_STANDALONE=YES"
        )
    if os.environ.get("RISC0_DEV_MODE") != "0":
        raise StandaloneQualificationError("RISC0_DEV_MODE must be exactly 0")
    profile = lez_wallet.load_network_profile(args.profile.resolve(strict=True))
    provenance = lez_wallet.verify_source(args.wallet_source, profile)
    execution = _execution_profile(args)
    results = (
        []
        if args.fresh
        else _resume_results(args.evidence, profile, provenance, execution)
    )
    selected = TESTS if not args.cases else tuple(TEST_BY_ID[case] for case in args.cases)
    for test in selected:
        print(f"running {test['id']}", file=sys.stderr, flush=True)
        result = run_test(Path(provenance["source"]), test, args.timeout, execution)
        results = [existing for existing in results if existing["id"] != test["id"]]
        results.append(result)
        lez_wallet.atomic_json(
            args.evidence, _artifact(profile, provenance, results, execution)
        )
    artifact = _artifact(profile, provenance, results, execution)
    lez_wallet.atomic_json(args.evidence, artifact)
    return artifact


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Pinned LEZ standalone qualification")
    result.add_argument("--profile", type=Path, default=lez_wallet.DEFAULT_PROFILE)
    result.add_argument("--wallet-source", type=Path, required=True)
    result.add_argument("--run", action="store_true")
    result.add_argument("--timeout", type=float, default=7200.0)
    result.add_argument(
        "--prover",
        choices=("ipc", "actor"),
        default="ipc",
        help="explicit local RISC Zero prover backend",
    )
    result.add_argument(
        "--rayon-threads",
        type=int,
        default=1,
        help="exact Rayon worker count recorded in evidence",
    )
    result.add_argument(
        "--case",
        dest="cases",
        action="append",
        choices=tuple(TEST_BY_ID),
        help="run one named case and resume compatible sanitized evidence",
    )
    result.add_argument("--fresh", action="store_true")
    result.add_argument(
        "--evidence",
        type=Path,
        default=REPO_ROOT / "evidence/standalone/official-wallet-real-proof.json",
    )
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.timeout <= 0:
            raise StandaloneQualificationError("timeout must be positive")
        if args.rayon_threads <= 0:
            raise StandaloneQualificationError("Rayon thread count must be positive")
        response = qualify(args)
        print(json.dumps(response, sort_keys=True))
        return 0
    except (StandaloneQualificationError, lez_wallet.WalletAdapterError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
