#!/usr/bin/env python3

"""Plan and sequentially execute the official-wallet Bonded lifecycle matrix."""

import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lez_bond = _load_tool("lez_bond")
lez_wallet = lez_bond.lez_wallet
CASES = (
    ("acceptance", "refund-accepted"),
    ("rejection", "sink-rejected"),
    ("delivery-failure", "refund-delivery-failed"),
    ("expiry", "refund-expired"),
)
OUTCOME_BY_CASE = dict(CASES)
STEP_ORDER = (
    ("expiry", "initialize"),
    ("acceptance", "initialize"),
    ("acceptance", "settle"),
    ("rejection", "initialize"),
    ("rejection", "settle"),
    ("delivery-failure", "initialize"),
    ("delivery-failure", "settle"),
    ("expiry", "settle"),
)
STEPS = tuple(
    (case, operation, OUTCOME_BY_CASE[case] if operation == "settle" else None)
    for case, operation in STEP_ORDER
)
HEX_32 = lez_bond.HEX_32


class MatrixError(RuntimeError):
    pass


def digest(domain: str, release_commit: str) -> str:
    return hashlib.sha256(f"Bonded/LEZ/v2/{release_commit}/{domain}".encode("ascii")).hexdigest()


def build_plan(args, now_ms: int) -> dict:
    if not lez_bond.re.fullmatch(r"[0-9a-f]{40}", args.release_commit):
        raise MatrixError("release commit must be a full lowercase Git SHA-1")
    if args.amount <= 0 or args.expiry_delay_ms <= 0 or args.standard_validity_ms <= 0:
        raise MatrixError("amount and validity durations must be positive")
    cases = {}
    for case, outcome in CASES:
        deadline = now_ms + (
            args.expiry_delay_ms if case == "expiry" else args.standard_validity_ms
        )
        cases[case] = {
            "bond_id": digest(f"{case}/bond", args.release_commit),
            "message_commitment": digest(f"{case}/message", args.release_commit),
            "policy_commitment": digest(f"{case}/policy", args.release_commit),
            "amount": args.amount,
            "deadline_ms": deadline,
            "outcome": outcome,
            "candidate_paths": {
                operation: str(
                    args.candidate_dir / f"{case}-{operation}-v2.json"
                )
                for operation in ("initialize", "settle")
            },
        }
    return {
        "schema_version": 1,
        "status": "planned",
        "network": "lez-testnet",
        "program_id": lez_bond.CANONICAL_PROGRAM_ID,
        "release_commit": args.release_commit,
        "accounts": {
            "sender": args.sender,
            "owner": args.owner,
            "sink": args.sink,
        },
        "proof_mode": "risc0-real-privacy-preserving-sequential",
        "execution": {
            "risc0_prover": args.prover,
            "rayon_num_threads": args.rayon_threads,
        },
        "created_at_utc": datetime.fromtimestamp(now_ms / 1000, timezone.utc)
        .replace(microsecond=0)
        .isoformat(),
        "cases": cases,
        "completed_steps": [],
    }


def _json_object(path: Path, label: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise MatrixError(f"{label} must be a regular non-symlink file")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MatrixError(f"could not read {label}: {exc}") from exc
    if not isinstance(document, dict):
        raise MatrixError(f"{label} must contain a JSON object")
    return document


def _validate_plan(plan: dict, args) -> None:
    if (
        plan.get("schema_version") != 1
        or plan.get("network") != "lez-testnet"
        or plan.get("program_id") != lez_bond.CANONICAL_PROGRAM_ID
        or plan.get("release_commit") != args.release_commit
        or plan.get("accounts")
        != {"sender": args.sender, "owner": args.owner, "sink": args.sink}
        or plan.get("execution")
        != {
            "risc0_prover": args.prover,
            "rayon_num_threads": args.rayon_threads,
        }
        or set(plan.get("cases", {})) != {case for case, _outcome in CASES}
    ):
        raise MatrixError("matrix journal does not match this exact release and account set")
    completed = plan.get("completed_steps")
    if not isinstance(completed, list) or len(completed) != len(set(completed)):
        raise MatrixError("matrix journal has invalid completed steps")
    valid_steps = {f"{case}-{operation}" for case, operation, _outcome in STEPS}
    if not set(completed) <= valid_steps:
        raise MatrixError("matrix journal contains an unknown completed step")


def _candidate(
    path: Path,
    case: str,
    operation: str,
    outcome: str | None,
    execution: dict,
) -> dict:
    candidate = _json_object(path, f"{case}-{operation} candidate")
    if (
        candidate.get("status") != "official-wallet-sequencer-finalized-candidate"
        or candidate.get("operation") != operation
        or candidate.get("outcome") != outcome
        or candidate.get("program_id") != lez_bond.CANONICAL_PROGRAM_ID
        or candidate.get("execution") != execution
        or candidate.get("transaction_type") != "PrivacyPreserving"
        or candidate.get("finality") != "Finalized"
    ):
        raise MatrixError(f"{case}-{operation} did not produce a finalized candidate")
    return candidate


def command(args, case: str, operation: str, values: dict) -> list[str]:
    result = [
        sys.executable,
        str(REPO_ROOT / "tools/lez_bond.py"),
        "--wallet-source",
        str(args.wallet_source),
        "--wallet-home",
        str(args.wallet_home),
        "--sender",
        args.sender,
        "--owner",
        args.owner,
        "--sink",
        args.sink,
        "--bond-id",
        values["bond_id"],
        "--timeout",
        str(args.timeout),
        "--prover",
        args.prover,
        "--rayon-threads",
        str(args.rayon_threads),
    ]
    if args.submit:
        result.append("--submit")
    result.append(operation)
    if operation == "initialize":
        result.extend(
            [
                "--message-commitment",
                values["message_commitment"],
                "--policy-commitment",
                values["policy_commitment"],
                "--amount",
                str(values["amount"]),
                "--deadline-ms",
                str(values["deadline_ms"]),
            ]
        )
    else:
        result.extend(["--outcome", values["outcome"]])
    result.extend(["--evidence", values["candidate_paths"][operation]])
    return result


def execute(args) -> dict:
    if args.submit:
        if os.environ.get("BONDED_LEZ_SUBMIT") != "YES":
            raise MatrixError("submission requires BONDED_LEZ_SUBMIT=YES")
        if os.environ.get("RISC0_DEV_MODE") != "0":
            raise MatrixError("RISC0_DEV_MODE must be exactly 0")
    if args.journal.exists():
        plan = _json_object(args.journal, "matrix journal")
    else:
        plan = build_plan(args, int(time.time() * 1000))
        lez_wallet.atomic_json(args.journal, plan)
    _validate_plan(plan, args)
    completed = set(plan["completed_steps"])
    visited = set(completed)
    execution = plan["execution"]
    for case, operation, outcome in STEPS:
        step = f"{case}-{operation}"
        values = plan["cases"][case]
        path = Path(values["candidate_paths"][operation])
        if step in completed:
            _candidate(path, case, operation, outcome, execution)
            continue
        if operation == "settle" and f"{case}-initialize" not in visited:
            raise MatrixError(f"{case} settlement cannot run before its initialization")
        if operation == "initialize" and int(time.time() * 1000) >= values["deadline_ms"]:
            raise MatrixError(f"{case} initialization validity window has expired")
        if args.submit and operation == "settle" and case == "expiry":
            remaining = values["deadline_ms"] - int(time.time() * 1000)
            if remaining > 0:
                if not args.wait_for_expiry:
                    raise MatrixError(
                        f"expiry settlement is {remaining} ms early; rerun with --wait-for-expiry"
                    )
                time.sleep(remaining / 1000)
        print(f"running {step}", file=sys.stderr, flush=True)
        process = subprocess.run(command(args, case, operation, values), cwd=REPO_ROOT)
        if process.returncode != 0:
            raise MatrixError(f"{step} failed with exit {process.returncode}")
        if not args.submit:
            visited.add(step)
            continue
        _candidate(path, case, operation, outcome, execution)
        completed.add(step)
        visited.add(step)
        plan["completed_steps"] = sorted(completed)
        plan["status"] = "completed" if len(completed) == len(STEPS) else "in-progress"
        plan["observed_at_utc"] = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
        lez_wallet.atomic_json(args.journal, plan)
    return plan


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Sequential official-wallet Bonded matrix")
    result.add_argument("--wallet-source", type=Path, required=True)
    result.add_argument("--wallet-home", type=Path, required=True)
    result.add_argument("--sender", required=True)
    result.add_argument("--owner", required=True)
    result.add_argument("--sink", required=True)
    result.add_argument("--release-commit", required=True)
    result.add_argument("--amount", type=int, default=10)
    result.add_argument("--standard-validity-ms", type=int, default=14 * 24 * 60 * 60 * 1000)
    result.add_argument("--expiry-delay-ms", type=int, default=8 * 60 * 60 * 1000)
    result.add_argument("--timeout", type=float, default=21600)
    result.add_argument("--prover", choices=("ipc", "actor"), default="ipc")
    result.add_argument("--rayon-threads", type=int, default=1)
    result.add_argument("--submit", action="store_true")
    result.add_argument("--wait-for-expiry", action="store_true")
    result.add_argument(
        "--candidate-dir",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates",
    )
    result.add_argument(
        "--journal",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates/bond-matrix-v2.json",
    )
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.timeout <= 0 or args.rayon_threads <= 0:
            raise MatrixError("timeout and Rayon thread count must be positive")
        response = execute(args)
        print(json.dumps(response, sort_keys=True))
        return 0
    except (MatrixError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
