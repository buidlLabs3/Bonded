#!/usr/bin/env python3

"""Read-only reconciliation of the official LEZ sequencer and explorer."""

import argparse
import base64
import hashlib
import json
import re
import struct
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit


SEQUENCER_URL = "https://testnet.lez.logos.co"
EXPLORER_URL = "https://explorer.testnet.lez.logos.co"
LEZ_RELEASE = "v0.2.4"
LEZ_RELEASE_COMMIT = "47eba256479f6f785acbd138834340703cd03401"
HEX_32 = re.compile(r"(?i)^[0-9a-f]{64}$")
HEX_40 = re.compile(r"(?i)^[0-9a-f]{40}$")
BEDROCK_STATUS = {0: "Pending", 1: "Safe", 2: "Finalized"}
TRANSACTION_VARIANTS = {0: "Public", 1: "PrivacyPreserving", 2: "ProgramDeployment"}


class ExplorerValidationError(RuntimeError):
    pass


def _require_official_url(value: str, expected: str) -> None:
    parsed = urlsplit(value)
    expected_parsed = urlsplit(expected)
    if (
        parsed.scheme != "https"
        or parsed.hostname != expected_parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
    ):
        raise ExplorerValidationError(f"service URL must be exactly {expected}")


def rpc_call(method: str, params: list, timeout: float = 30.0):
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params},
        separators=(",", ":"),
    ).encode("utf-8")
    request = urllib.request.Request(
        SEQUENCER_URL,
        data=body,
        headers={"content-type": "application/json", "user-agent": "bonded-lez-verifier/1"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
        raise ExplorerValidationError(f"sequencer RPC failed for {method}: {exc}") from exc
    if payload.get("error") is not None:
        raise ExplorerValidationError(f"sequencer RPC failed for {method}: {payload['error']}")
    if "result" not in payload:
        raise ExplorerValidationError(f"sequencer RPC omitted result for {method}")
    return payload["result"]


def fetch_explorer_page(path: str, timeout: float = 30.0) -> str:
    if not path.startswith("/") or path.startswith("//"):
        raise ExplorerValidationError("explorer path must be absolute and host-relative")
    request = urllib.request.Request(
        EXPLORER_URL + path,
        headers={"user-agent": "bonded-lez-verifier/1"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            final = urlsplit(response.geturl())
            if final.scheme != "https" or final.hostname != urlsplit(EXPLORER_URL).hostname:
                raise ExplorerValidationError("explorer redirected outside the official host")
            return response.read().decode("utf-8")
    except (OSError, urllib.error.URLError, UnicodeDecodeError) as exc:
        raise ExplorerValidationError(f"explorer request failed for {path}: {exc}") from exc


def hydration_resources(document: str) -> list:
    """Decode Leptos SSR resources without executing page JavaScript."""
    marker = "__RESOLVED_RESOURCES["
    decoder = json.JSONDecoder()
    resources = []
    position = 0
    while True:
        marker_at = document.find(marker, position)
        if marker_at < 0:
            break
        assignment_at = document.find("=", marker_at + len(marker))
        if assignment_at < 0:
            raise ExplorerValidationError("malformed explorer resource assignment")
        after_assignment = document[assignment_at + 1 :]
        encoded = after_assignment.lstrip()
        try:
            serialized, consumed = decoder.raw_decode(encoded)
            if not isinstance(serialized, str):
                raise TypeError("resource value is not a JSON string")
            resources.append(json.loads(serialized))
        except (json.JSONDecodeError, TypeError) as exc:
            raise ExplorerValidationError(f"invalid explorer hydration resource: {exc}") from exc
        position = assignment_at + 1 + len(after_assignment) - len(encoded) + consumed
    if not resources:
        raise ExplorerValidationError("explorer page contained no resolved resource")
    return resources


def _select_ok(document: str, predicate, label: str):
    errors = []
    for resource in hydration_resources(document):
        if not isinstance(resource, dict):
            continue
        if "Err" in resource:
            errors.append(resource["Err"])
            continue
        if "Ok" in resource and predicate(resource["Ok"]):
            return resource["Ok"]
    if errors:
        raise ExplorerValidationError(f"official explorer rejected {label}: {errors[0]}")
    raise ExplorerValidationError(f"official explorer omitted {label}")


def explorer_recent_blocks(document: str) -> list:
    def is_block_list(value):
        return isinstance(value, list) and value and all(
            isinstance(item, dict) and isinstance(item.get("header"), dict) for item in value
        )

    blocks = _select_ok(document, is_block_list, "recent finalized blocks")
    return sorted(blocks, key=lambda block: block["header"]["block_id"], reverse=True)


def explorer_block(document: str, block_id: int) -> dict:
    return _select_ok(
        document,
        lambda value: isinstance(value, dict)
        and isinstance(value.get("header"), dict)
        and value["header"].get("block_id") == block_id,
        f"block {block_id}",
    )


def transaction_kind_and_payload(value: dict) -> tuple[str, dict]:
    if not isinstance(value, dict) or len(value) != 1:
        raise ExplorerValidationError("explorer transaction has an unexpected shape")
    kind, payload = next(iter(value.items()))
    if kind not in TRANSACTION_VARIANTS.values() or not isinstance(payload, dict):
        raise ExplorerValidationError("explorer transaction has an unknown type")
    tx_hash = payload.get("hash")
    if not isinstance(tx_hash, str) or not HEX_32.fullmatch(tx_hash):
        raise ExplorerValidationError("explorer transaction has an invalid hash")
    return kind, payload


def explorer_transaction(document: str, tx_hash: str) -> dict:
    def is_transaction(value):
        try:
            _, payload = transaction_kind_and_payload(value)
            return payload["hash"].lower() == tx_hash.lower()
        except (ExplorerValidationError, KeyError):
            return False

    return _select_ok(document, is_transaction, f"transaction {tx_hash}")


def decode_sequencer_block(encoded: str) -> dict:
    try:
        block = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError) as exc:
        raise ExplorerValidationError(f"sequencer block is not valid base64: {exc}") from exc
    # v0.2.4 BlockHeader is u64 + 32 + 32 + u64 + 64 bytes. BlockBody starts
    # with its transaction count and BedrockStatus is the final enum byte.
    if len(block) < 149:
        raise ExplorerValidationError("sequencer block is shorter than the pinned v0.2.4 layout")
    status = BEDROCK_STATUS.get(block[-1])
    if status is None:
        raise ExplorerValidationError("sequencer block has an unknown BedrockStatus")
    return {
        "block_id": struct.unpack_from("<Q", block, 0)[0],
        "prev_block_hash": block[8:40].hex(),
        "hash": block[40:72].hex(),
        "timestamp": struct.unpack_from("<Q", block, 72)[0],
        "transaction_count": struct.unpack_from("<I", block, 144)[0],
        "bedrock_status": status,
    }


def decode_sequencer_transaction(encoded: str, containing_block: int) -> dict:
    try:
        transaction = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError) as exc:
        raise ExplorerValidationError(f"sequencer transaction is not valid base64: {exc}") from exc
    if not transaction:
        raise ExplorerValidationError("sequencer transaction is empty")
    kind = TRANSACTION_VARIANTS.get(transaction[0])
    if kind is None:
        raise ExplorerValidationError("sequencer transaction has an unknown variant")
    result = {"kind": kind, "block_id": containing_block}
    if kind == "ProgramDeployment":
        if len(transaction) < 5:
            raise ExplorerValidationError("deployment transaction is truncated")
        size = struct.unpack_from("<I", transaction, 1)[0]
        bytecode = transaction[5:]
        if size != len(bytecode):
            raise ExplorerValidationError("deployment bytecode length does not match Borsh prefix")
        result.update(
            {
                "bytecode_size": size,
                "bytecode_sha256": hashlib.sha256(bytecode).hexdigest(),
                "hash": hashlib.sha256(transaction[1:]).hexdigest(),
            }
        )
    return result


def _block_transactions(block: dict) -> list[dict]:
    body = block.get("body")
    transactions = body.get("transactions") if isinstance(body, dict) else None
    if not isinstance(transactions, list):
        raise ExplorerValidationError("explorer block omitted transactions")
    result = []
    for transaction in transactions:
        kind, payload = transaction_kind_and_payload(transaction)
        result.append({"kind": kind, "hash": payload["hash"].lower()})
    return result


def _require_rendered(document: str, expected: list[str], label: str) -> None:
    lowered = document.lower()
    if "transaction not found" in lowered or "block not found" in lowered:
        raise ExplorerValidationError(f"official explorer rendered not found for {label}")
    missing = [value for value in expected if value not in document]
    if missing:
        raise ExplorerValidationError(f"official explorer did not render {label}: {missing}")


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def reconcile(args) -> dict:
    _require_official_url(SEQUENCER_URL, SEQUENCER_URL)
    _require_official_url(EXPLORER_URL, EXPLORER_URL)
    tx_hash = args.tx_hash.lower()
    if not HEX_32.fullmatch(tx_hash):
        raise ExplorerValidationError("transaction hash must be 32-byte hex")
    if not HEX_32.fullmatch(args.bytecode_sha256):
        raise ExplorerValidationError("bytecode SHA-256 must be 32-byte hex")
    if not HEX_32.fullmatch(args.program_id):
        raise ExplorerValidationError("program ID must be 32-byte hex")
    if not HEX_40.fullmatch(args.release_commit):
        raise ExplorerValidationError("release commit must be 40-character hex")
    if args.overlap_count < 3 or args.confirmations < 1 or args.lag_scan < 1:
        raise ExplorerValidationError("overlap count must be >= 3 and bounds must be positive")

    observed_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
    rpc_call("checkHealth", [])
    channel_id = rpc_call("getChannelId", [])
    head_id = rpc_call("getLastBlockId", [])
    head = decode_sequencer_block(rpc_call("getBlock", [head_id]))

    root_document = fetch_explorer_page("/")
    recent = explorer_recent_blocks(root_document)
    explorer_tip = recent[0]
    explorer_tip_id = explorer_tip["header"]["block_id"]

    overlaps = []
    for indexed in recent[: args.overlap_count]:
        block_id = indexed["header"]["block_id"]
        sequenced = decode_sequencer_block(rpc_call("getBlock", [block_id]))
        overlaps.append(
            {
                "block_id": block_id,
                "sequencer_hash": sequenced["hash"],
                "explorer_hash": indexed["header"]["hash"].lower(),
                "sequencer_finality": sequenced["bedrock_status"],
                "explorer_finality": indexed["bedrock_status"],
                "matches": sequenced["hash"] == indexed["header"]["hash"].lower()
                and sequenced["bedrock_status"] == "Finalized"
                and indexed["bedrock_status"] == "Finalized",
            }
        )

    newest_sequencer_finalized = explorer_tip_id
    lag_scan_complete = True
    for block_id in range(explorer_tip_id + 1, min(head_id, explorer_tip_id + args.lag_scan) + 1):
        candidate = decode_sequencer_block(rpc_call("getBlock", [block_id]))
        if candidate["bedrock_status"] != "Finalized":
            break
        newest_sequencer_finalized = block_id
    else:
        if newest_sequencer_finalized < head_id:
            lag_scan_complete = False

    sequencer_tx_result = rpc_call("getTransaction", [tx_hash])
    if not isinstance(sequencer_tx_result, list) or len(sequencer_tx_result) != 2:
        raise ExplorerValidationError("sequencer did not return the candidate transaction")
    if not isinstance(sequencer_tx_result[1], int):
        raise ExplorerValidationError("sequencer transaction block ID is invalid")
    sequencer_tx = decode_sequencer_transaction(*sequencer_tx_result)
    sequencer_block = decode_sequencer_block(rpc_call("getBlock", [args.block_id]))

    block_path = f"/block/{args.block_id}"
    tx_path = f"/transaction/{tx_hash}"
    block_document = fetch_explorer_page(block_path)
    transaction_document = fetch_explorer_page(tx_path)
    indexed_block = explorer_block(block_document, args.block_id)
    indexed_transaction = explorer_transaction(transaction_document, tx_hash)
    indexed_kind, indexed_payload = transaction_kind_and_payload(indexed_transaction)
    indexed_transactions = _block_transactions(indexed_block)
    try:
        indexed_bytecode = base64.b64decode(
            indexed_payload["message"]["bytecode"], validate=True
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise ExplorerValidationError("explorer deployment bytecode is invalid") from exc
    indexed_bytecode_sha256 = hashlib.sha256(indexed_bytecode).hexdigest()

    _require_rendered(
        transaction_document,
        [tx_hash, "Program Deployment Transaction", f"{len(indexed_bytecode)} bytes"],
        "deployment transaction",
    )
    _require_rendered(
        block_document,
        [str(args.block_id), indexed_block["header"]["hash"], tx_hash, "Finalized"],
        "containing block",
    )

    confirmations = explorer_tip_id - args.block_id
    checks = {
        "three_or_more_finalized_overlaps_match": len(overlaps) >= 3
        and all(item["matches"] for item in overlaps),
        "explorer_reached_candidate_block": explorer_tip_id >= args.block_id,
        "sequencer_transaction_block_matches": sequencer_tx["block_id"] == args.block_id,
        "sequencer_transaction_hash_matches": sequencer_tx.get("hash") == tx_hash,
        "sequencer_transaction_type_matches": sequencer_tx["kind"] == args.transaction_type,
        "sequencer_block_id_matches": sequencer_block["block_id"] == args.block_id,
        "sequencer_block_is_finalized": sequencer_block["bedrock_status"] == "Finalized",
        "explorer_transaction_hash_matches": indexed_payload["hash"].lower() == tx_hash,
        "explorer_transaction_type_matches": indexed_kind == args.transaction_type,
        "explorer_block_hash_matches_sequencer": indexed_block["header"]["hash"].lower()
        == sequencer_block["hash"],
        "explorer_block_is_finalized": indexed_block["bedrock_status"] == "Finalized",
        "explorer_block_contains_transaction": {
            "kind": args.transaction_type,
            "hash": tx_hash,
        }
        in indexed_transactions,
        "bytecode_size_matches": sequencer_tx.get("bytecode_size") == args.bytecode_size
        == len(indexed_bytecode),
        "bytecode_digest_matches": sequencer_tx.get("bytecode_sha256")
        == args.bytecode_sha256.lower()
        == indexed_bytecode_sha256,
        "confirmation_depth_satisfied": confirmations >= args.confirmations,
    }
    passed = all(checks.values())
    if passed:
        status = "finalized"
    elif explorer_tip_id < args.block_id:
        status = "pending-indexer"
    else:
        status = "disputed"

    result = {
        "schema_version": 1,
        "status": status,
        "kind": "program-deployment",
        "component": "settlement-program",
        "network": "lez-testnet",
        "network_identity": {
            "channel_id": channel_id,
            "lez_release": LEZ_RELEASE,
            "lez_release_commit": LEZ_RELEASE_COMMIT,
        },
        "services": {
            "sequencer": SEQUENCER_URL,
            "explorer": EXPLORER_URL,
            "indexer": "server-side behind official explorer; no public endpoint discovered",
        },
        "transaction": tx_hash,
        "transaction_type": indexed_kind,
        "block": args.block_id,
        "block_hash": indexed_block["header"]["hash"].lower(),
        "finality": indexed_block["bedrock_status"],
        "program_id": args.program_id.lower(),
        "binary_sha256": indexed_bytecode_sha256,
        "binary_size": len(indexed_bytecode),
        "release_commit": args.release_commit.lower(),
        "observed_at_utc": observed_at,
        "confirmation_depth": confirmations,
        "required_confirmation_depth": args.confirmations,
        "explorer_urls": {
            "transaction": EXPLORER_URL + tx_path,
            "block": EXPLORER_URL + block_path,
        },
        "observations": {
            "sequencer_head": head,
            "explorer_latest_finalized": {
                "block_id": explorer_tip_id,
                "hash": explorer_tip["header"]["hash"].lower(),
                "timestamp": explorer_tip["header"]["timestamp"],
                "bedrock_status": explorer_tip["bedrock_status"],
            },
            "newest_sequencer_finalized_at_scan": newest_sequencer_finalized,
            "indexer_finalized_lag_blocks": newest_sequencer_finalized - explorer_tip_id,
            "lag_scan_complete": lag_scan_complete,
            "overlapping_finalized_blocks": overlaps,
        },
        "checks": checks,
        "verifier": {
            "command": "python3 tools/lez_explorer.py reconcile --evidence evidence/testnet/settlement-program.json",
            "source": "tools/lez_explorer.py",
            "source_commit": args.verifier_commit or None,
            "result": "pass" if passed else "fail",
        },
    }
    if args.evidence is not None:
        atomic_json(args.evidence, result)
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Verify canonical LEZ explorer evidence")
    commands = result.add_subparsers(dest="command", required=True)
    command = commands.add_parser("reconcile")
    command.add_argument("--tx-hash", default="d033cfe9a59a97824711f2a4d3df571281adc739e196cba1a7cf2264958298ad")
    command.add_argument("--block-id", type=int, default=4035)
    command.add_argument("--transaction-type", default="ProgramDeployment", choices=sorted(TRANSACTION_VARIANTS.values()))
    command.add_argument("--bytecode-sha256", default="8bca02c366c3261474e51818d8b2a95a441121c85bc57acf7bcc62dc8566182a")
    command.add_argument("--bytecode-size", type=int, default=370032)
    command.add_argument("--program-id", default="fb83bbb4c6140cb07e9a206d67e96a496bd395eed231e0f6158a672549e9a75c")
    command.add_argument("--release-commit", default="3c3054f59358864ca3ce93578e6d874778f1e230")
    command.add_argument("--verifier-commit", default="")
    command.add_argument("--confirmations", type=int, default=3)
    command.add_argument("--overlap-count", type=int, default=3)
    command.add_argument("--lag-scan", type=int, default=32)
    command.add_argument("--evidence", type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        report = reconcile(args)
        print(json.dumps(report, sort_keys=True))
        return 0 if report["status"] == "finalized" else 3
    except (ExplorerValidationError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
