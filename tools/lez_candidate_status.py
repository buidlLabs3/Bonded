#!/usr/bin/env python3

"""Read-only status audit for journaled LEZ wallet-provisioning transactions."""

import argparse
import importlib.util
import json
import sys
import time
from argparse import Namespace
from datetime import datetime, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
AUTHENTICATED_TRANSFER_PROGRAM_ID = (
    "fe96c4228babbe8bc578e3e25b884cacb07f8c86541f27ed676789875eef875a"
)
PINATA_PROGRAM_ID = "fc52f17a60f8b5e8de28e1a8c3133c012485011a36aef985ce24d69ff4f3528c"
PINATA_ACCOUNT = "EfQhKQAkX2FJiwNii2WFQsGndjvF1Mzd7RuVe7QdPLw7"
OPERATIONS = {
    "register:sender": ("register-sender", AUTHENTICATED_TRANSFER_PROGRAM_ID),
    "register:owner": ("register-owner", AUTHENTICATED_TRANSFER_PROGRAM_ID),
    "register:sink": ("register-sink", AUTHENTICATED_TRANSFER_PROGRAM_ID),
    "fund:sender": ("fund-sender", PINATA_PROGRAM_ID),
}
RETRYABLE_STATUSES = {"not-sequencer-included", "sequencer-included", "pending-indexer"}


def _load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lez_explorer = _load_tool("lez_explorer")


class CandidateStatusError(RuntimeError):
    pass


def load_candidate(path: Path) -> dict:
    try:
        document = json.loads(path.resolve(strict=True).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CandidateStatusError(f"could not read provisioning candidate: {exc}") from exc
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or document.get("network") != "lez-testnet"
        or document.get("release_commit") != lez_explorer.LEZ_RELEASE_COMMIT
        or not isinstance(document.get("accounts"), dict)
        or not isinstance(document.get("operations"), list)
    ):
        raise CandidateStatusError("provisioning candidate does not match the pinned LEZ testnet")
    accounts = document["accounts"]
    if (
        set(accounts) != {"sender", "owner", "sink"}
        or not all(isinstance(value, str) for value in accounts.values())
        or len(set(accounts.values())) != 3
    ):
        raise CandidateStatusError("provisioning candidate must contain three distinct role accounts")
    try:
        for account_id in accounts.values():
            lez_explorer.base58_decode(account_id, "candidate account ID")
    except lez_explorer.ExplorerValidationError as exc:
        raise CandidateStatusError(str(exc)) from exc
    return document


def select_operation(candidate: dict, operation_id: str) -> dict:
    matches = [item for item in candidate["operations"] if item.get("id") == operation_id]
    if len(matches) != 1:
        raise CandidateStatusError(f"candidate must contain exactly one {operation_id} operation")
    operation = matches[0]
    if operation.get("status") not in ("submitted", "finalized"):
        raise CandidateStatusError(f"{operation_id} has no unambiguous submitted transaction")
    transaction = operation.get("transaction")
    if not isinstance(transaction, str) or not lez_explorer.HEX_32.fullmatch(transaction):
        raise CandidateStatusError(f"{operation_id} has an invalid transaction hash")
    return operation


def _base_report(operation_id: str, transaction: str) -> dict:
    operation, _ = OPERATIONS[operation_id]
    return {
        "schema_version": 1,
        "scope": "read-only-candidate-status-not-release-evidence",
        "network": "lez-testnet",
        "component": "testnet-wallet",
        "kind": "wallet-registration" if operation_id.startswith("register:") else "wallet-funding",
        "operation": operation,
        "journal_operation": operation_id,
        "transaction": transaction,
        "observed_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "services": {
            "sequencer": lez_explorer.SEQUENCER_URL,
            "explorer": lez_explorer.EXPLORER_URL,
        },
    }


def audit(candidate: dict, operation_id: str, timeout: float, confirmations: int) -> dict:
    operation = select_operation(candidate, operation_id)
    transaction = operation["transaction"].lower()
    report = _base_report(operation_id, transaction)
    try:
        sequencer_result = lez_explorer.rpc_call("getTransaction", [transaction], timeout)
    except (lez_explorer.ExplorerValidationError, IndexError, KeyError, TypeError) as exc:
        return {**report, "status": "verification-unavailable", "reason": str(exc)}
    if sequencer_result is None:
        return {
            **report,
            "status": "not-sequencer-included",
            "reason": "the official sequencer does not currently return the exact journaled hash",
        }
    if (
        not isinstance(sequencer_result, list)
        or len(sequencer_result) != 2
        or not isinstance(sequencer_result[1], int)
    ):
        return {**report, "status": "disputed", "reason": "sequencer response has invalid shape"}
    try:
        raw_block = lez_explorer.rpc_call("getBlock", [sequencer_result[1]], timeout)
    except lez_explorer.ExplorerValidationError as exc:
        return {**report, "status": "verification-unavailable", "reason": str(exc)}
    try:
        decoded = lez_explorer.decode_sequencer_transaction(*sequencer_result)
        block = lez_explorer.decode_sequencer_block(raw_block)
    except (lez_explorer.ExplorerValidationError, KeyError, TypeError) as exc:
        return {**report, "status": "disputed", "reason": str(exc)}
    report.update(
        {
            "transaction_type": decoded["kind"],
            "block": decoded["block_id"],
            "block_hash": block["hash"],
            "finality": block["bedrock_status"],
            "serialized_transaction_sha256": decoded["hash"],
            "serialized_transaction_bytes": decoded["serialized_size"],
        }
    )
    if decoded["hash"] != transaction or decoded["kind"] != "Public":
        return {**report, "status": "disputed", "reason": "sequencer bytes or type mismatch"}
    if block["block_id"] != decoded["block_id"]:
        return {**report, "status": "disputed", "reason": "sequencer block ID mismatch"}
    if block["bedrock_status"] != "Finalized":
        return {**report, "status": "sequencer-included", "reason": "containing block is not finalized"}

    explorer_tip = None
    try:
        root_document = lez_explorer.fetch_explorer_page("/", timeout)
        explorer_tip = lez_explorer.explorer_recent_blocks(root_document)[0]["header"]["block_id"]
        transaction_document = lez_explorer.fetch_explorer_page(
            f"/transaction/{transaction}", timeout
        )
        indexed = lez_explorer.explorer_transaction(transaction_document, transaction)
        indexed_kind, indexed_payload = lez_explorer.transaction_kind_and_payload(indexed)
    except (lez_explorer.ExplorerValidationError, IndexError, KeyError, TypeError) as exc:
        message = str(exc)
        if explorer_tip is not None and (
            "not found" in message.lower() or "omitted transaction" in message.lower()
        ):
            status = "pending-indexer" if explorer_tip < decoded["block_id"] else "disputed"
            return {**report, "status": status, "explorer_tip": explorer_tip, "reason": message}
        return {**report, "status": "verification-unavailable", "reason": message}

    role = operation_id.split(":", 1)[1]
    expected_accounts = [candidate["accounts"][role]]
    if operation_id == "fund:sender":
        expected_accounts.insert(0, PINATA_ACCOUNT)
    expected_program = OPERATIONS[operation_id][1]
    args = Namespace(
        tx_hash=transaction,
        block_id=decoded["block_id"],
        transaction_type="Public",
        kind=report["kind"],
        component="testnet-wallet",
        operation=report["operation"],
        program_id=expected_program,
        account_id=expected_accounts,
        verifier_commit="",
        observer="candidate-status",
        confirmations=confirmations,
        overlap_count=3,
        lag_scan=32,
        evidence=None,
    )
    try:
        reconciled = lez_explorer.reconcile_transaction(args)
    except lez_explorer.ExplorerValidationError as exc:
        return {**report, "status": "verification-unavailable", "reason": str(exc)}
    return {
        **report,
        "status": reconciled["status"],
        "explorer_tip": explorer_tip,
        "explorer_transaction_type": indexed_kind,
        "explorer_program_id": indexed_payload.get("message", {}).get("program_id"),
        "checks": reconciled["checks"],
        "explorer_urls": reconciled["explorer_urls"],
    }


def wait_for_status(
    candidate: dict,
    operation_id: str,
    timeout: float,
    confirmations: int,
    wait_seconds: float,
    interval: float,
) -> dict:
    deadline = time.monotonic() + wait_seconds
    attempts = 0
    while True:
        attempts += 1
        report = audit(candidate, operation_id, timeout, confirmations)
        report["attempts"] = attempts
        if report["status"] not in RETRYABLE_STATUSES or time.monotonic() >= deadline:
            if report["status"] in RETRYABLE_STATUSES and wait_seconds > 0:
                report["wait_deadline_reached"] = True
            return report
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            report["wait_deadline_reached"] = True
            return report
        time.sleep(min(interval, remaining))


def write_new(path: Path, report: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Read-only LEZ provisioning candidate status")
    result.add_argument("--candidate", type=Path, required=True)
    result.add_argument("--operation", choices=tuple(OPERATIONS), required=True)
    result.add_argument("--timeout", type=float, default=20.0)
    result.add_argument("--confirmations", type=int, default=3)
    result.add_argument("--wait", type=float, default=0.0, help="bounded total polling seconds")
    result.add_argument("--interval", type=float, default=12.0)
    result.add_argument("--output", type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.timeout <= 0 or args.confirmations < 1 or args.wait < 0 or args.interval <= 0:
            raise CandidateStatusError(
                "timeout, confirmations, and interval must be positive; wait must be non-negative"
            )
        report = wait_for_status(
            load_candidate(args.candidate),
            args.operation,
            args.timeout,
            args.confirmations,
            args.wait,
            args.interval,
        )
        if args.output is not None:
            write_new(args.output, report)
        print(json.dumps(report, sort_keys=True))
        return 0 if report["status"] == "finalized" else 3
    except (CandidateStatusError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
