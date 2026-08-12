#!/usr/bin/env python3

"""Promote a finalized LEZ provisioning journal through the public verifier."""

import argparse
import importlib.util
import json
import sys
from argparse import Namespace
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


candidate_status = _load_tool("lez_candidate_status")
lez_explorer = _load_tool("lez_explorer")


class WalletEvidenceError(RuntimeError):
    pass


def promote(args) -> dict:
    candidate = candidate_status.load_candidate(args.candidate)
    operation = candidate_status.select_operation(candidate, args.operation)
    if operation.get("status") != "finalized":
        raise WalletEvidenceError(
            f"{args.operation} is not finalized in the provisioning journal"
        )
    if (
        operation.get("transaction_type") != "Public"
        or operation.get("finality") != "Finalized"
        or not isinstance(operation.get("block"), int)
        or isinstance(operation.get("block"), bool)
        or operation["block"] < 0
    ):
        raise WalletEvidenceError(
            f"{args.operation} lacks canonical Public transaction finality"
        )

    role = args.operation.split(":", 1)[1]
    expected_accounts = [candidate["accounts"][role]]
    if args.operation == "fund:sender":
        expected_accounts.insert(0, candidate_status.PINATA_ACCOUNT)
    operation_name, program_id = candidate_status.OPERATIONS[args.operation]
    verifier_args = Namespace(
        tx_hash=operation["transaction"],
        block_id=operation["block"],
        transaction_type="Public",
        kind=(
            "wallet-registration"
            if args.operation.startswith("register:")
            else "wallet-funding"
        ),
        component="testnet-wallet",
        operation=operation_name,
        program_id=program_id,
        account_id=expected_accounts,
        verifier_commit=args.verifier_commit,
        observer=args.observer,
        confirmations=args.confirmations,
        overlap_count=3,
        lag_scan=32,
        evidence=None,
    )
    result = lez_explorer.reconcile_transaction(verifier_args)
    if result["status"] != "finalized" or not all(result["checks"].values()):
        raise WalletEvidenceError(
            f"{args.operation} did not pass the finalized public explorer contract"
        )
    if (
        result["block_hash"] != operation.get("block_hash")
        or result["transaction"] != operation.get("transaction")
    ):
        raise WalletEvidenceError(
            f"{args.operation} explorer result conflicts with the provisioning journal"
        )
    lez_explorer.atomic_json(args.evidence, result)
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Promote finalized LEZ wallet provisioning evidence read-only"
    )
    result.add_argument("--candidate", type=Path, required=True)
    result.add_argument("--operation", choices=tuple(candidate_status.OPERATIONS), required=True)
    result.add_argument("--verifier-commit", required=True)
    result.add_argument("--observer", required=True)
    result.add_argument("--confirmations", type=int, default=3)
    result.add_argument("--evidence", type=Path, required=True)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.confirmations < 3:
            raise WalletEvidenceError("at least three confirmations are required")
        report = promote(args)
        print(json.dumps(report, sort_keys=True))
        return 0
    except (
        WalletEvidenceError,
        candidate_status.CandidateStatusError,
        lez_explorer.ExplorerValidationError,
        OSError,
        ValueError,
    ) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
