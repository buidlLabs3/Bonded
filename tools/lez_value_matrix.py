#!/usr/bin/env python3

"""Validate and sequentially execute the three authorized LEZ value transfers."""

import argparse
import hashlib
import importlib.util
import json
import os
import re
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


lez_value = _load_tool("lez_value_transfer")
lez_wallet = lez_value.lez_wallet
HEX_32 = re.compile(r"(?i)^[0-9a-f]{64}$")
OPERATION_SPECS = (
    ("below-limit-transfer", "owner", 2),
    ("owner-approved-transfer", "owner", 3),
    ("paid-task-settlement", "sink", 4),
)


class ValueMatrixError(RuntimeError):
    pass


def _json_object(path: Path, label: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise ValueMatrixError(f"{label} must be a regular non-symlink file")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueMatrixError(f"could not read {label}: {exc}") from exc
    if not isinstance(document, dict):
        raise ValueMatrixError(f"{label} must contain a JSON object")
    return document


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def operation_bindings(args, now_ms: int) -> dict:
    bindings = {}
    for operation, recipient_role, amount in OPERATION_SPECS:
        authorization = args.authorization_dir / f"{operation}.json"
        trusted_signers = args.authorization_dir / f"{operation}-trusted-signers.json"
        document = _json_object(authorization, f"{operation} authorization")
        trusted = lez_value.load_trusted_signers(trusted_signers)
        payload = lez_value.validate_authorization(document, now_ms, trusted)
        recipient = getattr(args, recipient_role)
        expected = {
            "operation": operation,
            "profile": "settlement",
            "sender": args.sender,
            "recipient": recipient,
            "amount": amount,
        }
        if any(payload.get(field) != value for field, value in expected.items()):
            raise ValueMatrixError(
                f"{operation} signed payload does not match the release account and amount plan"
            )
        bindings[operation] = {
            **expected,
            "authorization": str(authorization),
            "authorization_sha256": _sha256(authorization),
            "authorization_payload_sha256": document["payload_sha256"],
            "trusted_signers": str(trusted_signers),
            "trusted_signers_sha256": _sha256(trusted_signers),
            "valid_until_ms": payload["expires_at_ms"],
            "candidate": str(args.candidate_dir / f"{operation}-v2.json"),
        }
    return bindings


def build_plan(args, now_ms: int) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", args.release_commit):
        raise ValueMatrixError("release commit must be a full lowercase Git SHA-1")
    if len({args.sender, args.owner, args.sink}) != 3 or any(
        not value for value in (args.sender, args.owner, args.sink)
    ):
        raise ValueMatrixError("sender, owner, and sink must be distinct nonempty accounts")
    if args.prover not in ("ipc", "actor") or args.rayon_threads <= 0:
        raise ValueMatrixError("execution must use a local prover and positive thread count")
    return {
        "schema_version": 1,
        "status": "planned",
        "network": "lez-testnet",
        "release_commit": args.release_commit,
        "accounts": {
            "sender": args.sender,
            "owner": args.owner,
            "sink": args.sink,
        },
        "execution": {
            "risc0_prover": args.prover,
            "rayon_num_threads": args.rayon_threads,
        },
        "proof_mode": "risc0-real-privacy-preserving-sequential",
        "created_at_utc": datetime.fromtimestamp(now_ms / 1000, timezone.utc)
        .replace(microsecond=0)
        .isoformat(),
        "operations": operation_bindings(args, now_ms),
        "completed_operations": [],
    }


def _validate_plan(plan: dict, args, now_ms: int) -> None:
    operation_ids = [operation for operation, _role, _amount in OPERATION_SPECS]
    completed = plan.get("completed_operations")
    if (
        plan.get("schema_version") != 1
        or plan.get("network") != "lez-testnet"
        or plan.get("release_commit") != args.release_commit
        or plan.get("accounts")
        != {"sender": args.sender, "owner": args.owner, "sink": args.sink}
        or plan.get("execution")
        != {
            "risc0_prover": args.prover,
            "rayon_num_threads": args.rayon_threads,
        }
        or plan.get("operations") != operation_bindings(args, now_ms)
    ):
        raise ValueMatrixError(
            "value matrix journal does not match the signed release transfer plan"
        )
    if not isinstance(completed, list) or completed != operation_ids[: len(completed)]:
        raise ValueMatrixError("value matrix completed operations are not an ordered prefix")
    expected_status = (
        "planned"
        if not completed
        else "completed"
        if len(completed) == len(operation_ids)
        else "in-progress"
    )
    if plan.get("status") != expected_status:
        raise ValueMatrixError("value matrix status does not match its completed operations")


def command(args, values: dict) -> list[str]:
    result = [
        sys.executable,
        str(REPO_ROOT / "tools/lez_value_transfer.py"),
        "execute",
        "--network-profile",
        str(args.network_profile),
        "--wallet-source",
        str(args.wallet_source),
        "--wallet-home",
        str(args.wallet_home),
        "--operation",
        values["operation"],
        "--profile",
        values["profile"],
        "--sender",
        values["sender"],
        "--recipient",
        values["recipient"],
        "--amount",
        str(values["amount"]),
        "--authorization",
        values["authorization"],
        "--trusted-signers",
        values["trusted_signers"],
        "--timeout",
        str(args.timeout),
        "--prover",
        args.prover,
        "--rayon-threads",
        str(args.rayon_threads),
        "--evidence",
        values["candidate"],
    ]
    if args.submit:
        result.append("--submit")
    return result


def _candidate(path: Path, values: dict, execution: dict) -> dict:
    candidate = _json_object(path, f"{values['operation']} candidate")
    transaction = str(candidate.get("transaction", ""))
    authorization = candidate.get("authorization")
    state = candidate.get("state")
    if (
        candidate.get("status") != "official-wallet-sequencer-finalized-candidate"
        or candidate.get("operation") != values["operation"]
        or candidate.get("network") != "lez-testnet"
        or candidate.get("profile") != values["profile"]
        or candidate.get("program_id")
        != lez_value.AUTHENTICATED_TRANSFER_PROGRAM_ID
        or candidate.get("accounts")
        != {"sender": values["sender"], "recipient": values["recipient"]}
        or candidate.get("amount") != values["amount"]
        or candidate.get("authorization_sha256") != values["authorization_sha256"]
        or not isinstance(authorization, dict)
        or authorization.get("payload_sha256")
        != values["authorization_payload_sha256"]
        or candidate.get("trusted_signers_sha256")
        != values["trusted_signers_sha256"]
        or candidate.get("execution") != execution
        or candidate.get("transaction_type") != "PrivacyPreserving"
        or candidate.get("finality") != "Finalized"
        or not HEX_32.fullmatch(transaction)
        or candidate.get("serialized_transaction_sha256") != transaction
        or not isinstance(candidate.get("block"), int)
        or isinstance(candidate.get("block"), bool)
        or candidate["block"] <= 0
        or not HEX_32.fullmatch(str(candidate.get("block_hash", "")))
        or not isinstance(state, dict)
        or not isinstance(state.get("before"), dict)
        or not isinstance(state.get("after"), dict)
    ):
        raise ValueMatrixError(
            f"{values['operation']} did not produce the exact finalized candidate"
        )
    return candidate


def execute(args) -> dict:
    if args.submit:
        if os.environ.get("BONDED_LEZ_SUBMIT") != "YES":
            raise ValueMatrixError("submission requires BONDED_LEZ_SUBMIT=YES")
        if os.environ.get("RISC0_DEV_MODE") != "0":
            raise ValueMatrixError("RISC0_DEV_MODE must be exactly 0")
    now_ms = int(time.time() * 1000)
    if args.journal.exists():
        plan = _json_object(args.journal, "value matrix journal")
    else:
        plan = build_plan(args, now_ms)
        lez_wallet.atomic_json(args.journal, plan)
    _validate_plan(plan, args, now_ms)
    completed = plan["completed_operations"]
    execution = plan["execution"]
    for operation, _recipient_role, _amount in OPERATION_SPECS:
        values = plan["operations"][operation]
        path = Path(values["candidate"])
        if operation in completed:
            _candidate(path, values, execution)
            continue
        print(f"running {operation}", file=sys.stderr, flush=True)
        process = subprocess.run(command(args, values), cwd=REPO_ROOT)
        if process.returncode != 0:
            raise ValueMatrixError(
                f"{operation} failed with exit {process.returncode}"
            )
        if not args.submit:
            continue
        _candidate(path, values, execution)
        completed.append(operation)
        plan["status"] = (
            "completed"
            if len(completed) == len(OPERATION_SPECS)
            else "in-progress"
        )
        plan["observed_at_utc"] = datetime.now(timezone.utc).replace(
            microsecond=0
        ).isoformat()
        lez_wallet.atomic_json(args.journal, plan)
    return plan


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Sequential official-wallet LEZ value transfer matrix"
    )
    result.add_argument(
        "--network-profile", type=Path, default=lez_wallet.DEFAULT_PROFILE
    )
    result.add_argument("--wallet-source", type=Path, required=True)
    result.add_argument("--wallet-home", type=Path, required=True)
    result.add_argument("--sender", required=True)
    result.add_argument("--owner", required=True)
    result.add_argument("--sink", required=True)
    result.add_argument("--release-commit", required=True)
    result.add_argument("--timeout", type=float, default=21600)
    result.add_argument("--prover", choices=("ipc", "actor"), default="ipc")
    result.add_argument("--rayon-threads", type=int, default=1)
    result.add_argument("--submit", action="store_true")
    result.add_argument(
        "--authorization-dir",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/authorizations",
    )
    result.add_argument(
        "--candidate-dir",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates",
    )
    result.add_argument(
        "--journal",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates/value-matrix-v2.json",
    )
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.timeout <= 0 or args.rayon_threads <= 0:
            raise ValueMatrixError("timeout and Rayon thread count must be positive")
        response = execute(args)
        print(json.dumps(response, sort_keys=True))
        return 0
    except (
        ValueMatrixError,
        lez_value.ValueTransferError,
        lez_value.lez_bond.BondAdapterError,
        lez_wallet.WalletAdapterError,
        OSError,
        ValueError,
    ) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
