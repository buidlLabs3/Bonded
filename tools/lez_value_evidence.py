#!/usr/bin/env python3

"""Promote a finalized authorized value transfer through the public LEZ verifier."""

import argparse
import hashlib
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


lez_value = _load_tool("lez_value_transfer")
lez_explorer = lez_value.lez_bond.lez_explorer


class ValueEvidenceError(RuntimeError):
    pass


def _json_object(path: Path, label: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise ValueEvidenceError(f"{label} must be a regular non-symlink file")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueEvidenceError(f"could not read {label}: {exc}") from exc
    if not isinstance(document, dict):
        raise ValueEvidenceError(f"{label} must contain a JSON object")
    return document


def promote(args) -> dict:
    candidate = _json_object(args.candidate, "value-transfer journal")
    authorization = _json_object(args.authorization, "authorization")
    trusted_signers = lez_value.load_trusted_signers(args.trusted_signers)
    payload = lez_value.validate_authorization(authorization, trusted_signers=trusted_signers)
    expected = lez_value.OPERATIONS[args.operation]
    if (
        candidate.get("status") != "official-wallet-sequencer-finalized-candidate"
        or candidate.get("operation") != args.operation
        or candidate.get("network") != "lez-testnet"
        or candidate.get("program_id") != lez_value.AUTHENTICATED_TRANSFER_PROGRAM_ID
        or candidate.get("transaction_type") != "PrivacyPreserving"
        or candidate.get("finality") != "Finalized"
    ):
        raise ValueEvidenceError("value-transfer journal is not a canonical finalized candidate")
    if payload.get("operation") != args.operation:
        raise ValueEvidenceError("authorization operation does not match the promoted row")
    accounts = candidate.get("accounts")
    if not isinstance(accounts, dict) or accounts.get("sender") == accounts.get("recipient"):
        raise ValueEvidenceError("value-transfer journal account map is invalid")
    if any(
        candidate.get(field) != payload.get(field)
        for field in ("operation", "profile", "amount")
    ) or accounts != {"sender": payload.get("sender"), "recipient": payload.get("recipient")}:
        raise ValueEvidenceError("signed authorization conflicts with the value-transfer journal")
    if candidate.get("authorization_sha256") != hashlib.sha256(
        args.authorization.read_bytes()
    ).hexdigest() or candidate.get("trusted_signers_sha256") != hashlib.sha256(
        args.trusted_signers.read_bytes()
    ).hexdigest():
        raise ValueEvidenceError("authorization artifact digest conflicts with the journal")
    state = candidate.get("state")
    if not isinstance(state, dict) or not all(
        isinstance(state.get(stage), dict) for stage in ("before", "after")
    ):
        raise ValueEvidenceError("value-transfer journal omits before/after state")
    try:
        before = state["before"]
        after = state["after"]
        amount = candidate["amount"]
        sender_delta = int(before["sender"]["balance"]) - int(after["sender"]["balance"])
        recipient_delta = int(after["recipient"]["balance"]) - int(
            before["recipient"]["balance"]
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueEvidenceError("value-transfer journal state is malformed") from exc
    if sender_delta != amount or recipient_delta != amount:
        raise ValueEvidenceError("value-transfer journal state does not conserve the exact amount")
    verifier_args = Namespace(
        tx_hash=candidate["transaction"],
        block_id=candidate["block"],
        transaction_type="PrivacyPreserving",
        kind=expected["kind"],
        component=expected["component"],
        operation=args.operation,
        program_id=None,
        account_id=[accounts["sender"], accounts["recipient"]],
        verifier_commit=args.verifier_commit,
        observer=args.observer,
        confirmations=args.confirmations,
        overlap_count=3,
        lag_scan=32,
        evidence=None,
    )
    result = lez_explorer.reconcile_transaction(verifier_args)
    if result["status"] != "finalized" or not all(result["checks"].values()):
        raise ValueEvidenceError("value transfer did not pass the public explorer contract")
    if any(
        result.get(field) != candidate.get(field)
        for field in ("transaction", "block", "block_hash", "transaction_type")
    ):
        raise ValueEvidenceError("explorer result conflicts with the value-transfer journal")
    result["value_transfer_provenance"] = {
        "profile": candidate["profile"],
        "program_id": candidate["program_id"],
        "amount": candidate["amount"],
        "authorization": candidate["authorization"],
        "authorization_sha256": candidate["authorization_sha256"],
        "trusted_signers_sha256": candidate["trusted_signers_sha256"],
        "instruction_words_sha256": candidate["instruction_words_sha256"],
        "journal_sha256": hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
        "state_reconciliation": "validated-before-and-after-by-tools/lez_value_transfer.py",
    }
    lez_explorer.atomic_json(args.evidence, result)
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Promote a finalized LEZ value transfer")
    result.add_argument("--candidate", type=Path, required=True)
    result.add_argument("--authorization", type=Path, required=True)
    result.add_argument("--trusted-signers", type=Path, required=True)
    result.add_argument("--operation", choices=tuple(lez_value.OPERATIONS), required=True)
    result.add_argument("--verifier-commit", required=True)
    result.add_argument("--observer", required=True)
    result.add_argument("--confirmations", type=int, default=3)
    result.add_argument("--evidence", type=Path, required=True)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.confirmations < 3:
            raise ValueEvidenceError("at least three confirmations are required")
        report = promote(args)
        print(json.dumps(report, sort_keys=True))
        return 0
    except (ValueEvidenceError, lez_explorer.ExplorerValidationError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
