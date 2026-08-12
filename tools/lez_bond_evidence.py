#!/usr/bin/env python3

"""Promote a finalized Bonded lifecycle journal through the public LEZ verifier."""

import argparse
import hashlib
import importlib.util
import json
import sys
from argparse import Namespace
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEPLOYMENT = REPO_ROOT / "evidence/testnet/settlement-program.json"


def _load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lez_bond = _load_tool("lez_bond")
lez_explorer = _load_tool("lez_explorer")

OPERATIONS = {
    "acceptance-initialize": ("initialize", None),
    "acceptance-settle": ("settle", "refund-accepted"),
    "rejection-initialize": ("initialize", None),
    "rejection-settle": ("settle", "sink-rejected"),
    "expiry-initialize": ("initialize", None),
    "expiry-settle": ("settle", "refund-expired"),
    "delivery-failure-initialize": ("initialize", None),
    "delivery-failure-settle": ("settle", "refund-delivery-failed"),
}


class BondEvidenceError(RuntimeError):
    pass


def _json_object(path: Path, label: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise BondEvidenceError(f"{label} must be a regular non-symlink file")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BondEvidenceError(f"could not read {label}: {exc}") from exc
    if not isinstance(document, dict):
        raise BondEvidenceError(f"{label} must contain a JSON object")
    return document


def _expected_accounts(candidate: dict, operation: str) -> list[str]:
    accounts = candidate.get("accounts")
    if not isinstance(accounts, dict):
        raise BondEvidenceError("lifecycle journal omits its public account map")
    names = (
        ("sender", "state", "escrow", "clock")
        if operation == "initialize"
        else ("state", "escrow", "destination", "owner", "clock")
    )
    try:
        expected = [accounts[name] for name in names]
    except KeyError as exc:
        raise BondEvidenceError(f"lifecycle journal omits account {exc.args[0]}") from exc
    if not all(isinstance(value, str) for value in expected) or len(set(expected)) != len(
        expected
    ):
        raise BondEvidenceError("lifecycle public accounts must be distinct strings")
    if operation == "settle":
        outcome = candidate.get("outcome")
        destination_role = "sink" if outcome == "sink-rejected" else "sender"
        if accounts.get("destination") != accounts.get(destination_role):
            raise BondEvidenceError("lifecycle settlement destination conflicts with its outcome")
    return expected


def _validate_candidate(candidate: dict, deployment: dict, inventory_operation: str) -> list[str]:
    operation, outcome = OPERATIONS[inventory_operation]
    if candidate.get("status") != "official-wallet-sequencer-finalized-candidate":
        raise BondEvidenceError("lifecycle journal is not a finalized official-wallet candidate")
    if candidate.get("operation") != operation or candidate.get("outcome") != outcome:
        raise BondEvidenceError("lifecycle journal does not match the requested inventory operation")
    if (
        candidate.get("network") != "lez-testnet"
        or candidate.get("network_identity") != deployment.get("network_identity")
    ):
        raise BondEvidenceError("lifecycle journal does not match the canonical testnet identity")
    for field in ("program_id", "binary_sha256", "binary_size"):
        if candidate.get(field) != deployment.get(field):
            raise BondEvidenceError(f"lifecycle journal {field} is not the canonical deployment")
    if candidate.get("transaction_type") != "PrivacyPreserving" or candidate.get(
        "finality"
    ) != "Finalized":
        raise BondEvidenceError("lifecycle journal lacks canonical privacy transaction finality")
    if not lez_explorer.HEX_32.fullmatch(str(candidate.get("transaction", ""))):
        raise BondEvidenceError("lifecycle journal has an invalid transaction hash")
    if not lez_explorer.HEX_32.fullmatch(str(candidate.get("block_hash", ""))):
        raise BondEvidenceError("lifecycle journal has an invalid block hash")
    block = candidate.get("block")
    if not isinstance(block, int) or isinstance(block, bool) or block < 0:
        raise BondEvidenceError("lifecycle journal has an invalid block number")
    if not isinstance(candidate.get("state", {}).get("after"), dict):
        raise BondEvidenceError("lifecycle journal omits reconciled post-call state")
    return _expected_accounts(candidate, operation)


def promote(args) -> dict:
    candidate = _json_object(args.candidate, "lifecycle journal")
    deployment = _json_object(args.deployment, "canonical deployment evidence")
    expected_accounts = _validate_candidate(candidate, deployment, args.operation)
    verifier_args = Namespace(
        tx_hash=candidate["transaction"],
        block_id=candidate["block"],
        transaction_type="PrivacyPreserving",
        kind="bond-lifecycle",
        component="settlement-program",
        operation=args.operation,
        program_id=None,
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
        raise BondEvidenceError("lifecycle call did not pass the public explorer contract")
    if any(
        result.get(field) != candidate.get(field)
        for field in ("transaction", "block", "block_hash", "transaction_type")
    ):
        raise BondEvidenceError("explorer result conflicts with the lifecycle journal")
    result["bond_provenance"] = {
        "bond_id": candidate["bond_id"],
        "program_id": candidate["program_id"],
        "binary_sha256": candidate["binary_sha256"],
        "binary_size": candidate["binary_size"],
        "instruction_word_count": candidate["instruction_word_count"],
        "instruction_words_sha256": candidate["instruction_words_sha256"],
        "outcome": candidate.get("outcome"),
        "journal_sha256": hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
        "state_reconciliation": "validated-before-and-after-by-tools/lez_bond.py",
    }
    lez_explorer.atomic_json(args.evidence, result)
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Promote finalized Bonded lifecycle evidence read-only"
    )
    result.add_argument("--candidate", type=Path, required=True)
    result.add_argument("--deployment", type=Path, default=DEFAULT_DEPLOYMENT)
    result.add_argument("--operation", choices=tuple(OPERATIONS), required=True)
    result.add_argument("--verifier-commit", required=True)
    result.add_argument("--observer", required=True)
    result.add_argument("--confirmations", type=int, default=3)
    result.add_argument("--evidence", type=Path, required=True)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.confirmations < 3:
            raise BondEvidenceError("at least three confirmations are required")
        report = promote(args)
        print(json.dumps(report, sort_keys=True))
        return 0
    except (
        BondEvidenceError,
        lez_explorer.ExplorerValidationError,
        OSError,
        ValueError,
    ) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
