#!/usr/bin/env python3

"""Fail-closed, read-only audit of public LEZ testnet transaction evidence."""

import argparse
import hashlib
import importlib.util
import json
import re
import sys
from argparse import Namespace
from datetime import datetime, timedelta, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INDEX = REPO_ROOT / "evidence/testnet/required-evidence.json"
VERIFIER_VERSION = "1.0.0"
MAX_LATEST_OBSERVATION_AGE = timedelta(days=7)
HEX_32 = re.compile(r"^[0-9a-f]{64}$")
HEX_40 = re.compile(r"^[0-9a-f]{40}$")
SLUG = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
EXPECTED_NETWORK = {
    "network": "lez-testnet",
    "channel_id": "01" * 32,
    "lez_release": "v0.2.4",
    "lez_release_commit": "47eba256479f6f785acbd138834340703cd03401",
    "sequencer": "https://testnet.lez.logos.co",
    "explorer": "https://explorer.testnet.lez.logos.co",
}
TRANSACTION_TITLES = {
    "Public": "Public Transaction",
    "PrivacyPreserving": "Privacy-Preserving Transaction",
    "ProgramDeployment": "Program Deployment Transaction",
}
SENSITIVE_KEYS = {
    "attachment_content",
    "ciphertext",
    "encrypted_post_state",
    "encrypted_payload",
    "identity_secret",
    "isk",
    "message_content",
    "mnemonic",
    "nsk",
    "plaintext",
    "private_key",
    "private_actions",
    "proof",
    "proof_data",
    "recovery_phrase",
    "seed",
    "secret",
    "secret_key",
    "vsk",
    "wallet_history",
    "witness",
    "witness_set",
}
SENSITIVE_TEXT = (
    "recovery phrase:",
    "mnemonic phrase:",
    "private key:",
    "secret key:",
    " nsk:",
    " isk:",
)


class EvidenceGateError(RuntimeError):
    pass


class VerificationUnavailable(EvidenceGateError):
    pass


def _load_explorer():
    spec = importlib.util.spec_from_file_location(
        "lez_explorer_gate", REPO_ROOT / "tools/lez_explorer.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceGateError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except EvidenceGateError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceGateError(f"could not parse {path}: {exc}") from exc


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def evidence_path(root: Path, value: str) -> Path:
    if not isinstance(value, str) or not value.startswith("evidence/testnet/"):
        raise EvidenceGateError("artifact path must be repository-relative under evidence/testnet")
    candidate = root / value
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as exc:
        raise EvidenceGateError(f"required artifact is missing: {value}") from exc
    evidence_root = (root / "evidence/testnet").resolve(strict=True)
    if resolved != evidence_root and evidence_root not in resolved.parents:
        raise EvidenceGateError(f"artifact escapes evidence/testnet: {value}")
    if candidate.is_symlink() or not resolved.is_file():
        raise EvidenceGateError(f"artifact must be a regular non-symlink file: {value}")
    return resolved


def _scan_sensitive(value, location="$") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = str(key).lower().replace("-", "_")
            if normalized in SENSITIVE_KEYS:
                raise EvidenceGateError(f"sensitive evidence field is forbidden: {location}.{key}")
            _scan_sensitive(child, f"{location}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _scan_sensitive(child, f"{location}[{index}]")
    elif isinstance(value, str):
        lowered = value.lower()
        if any(marker in lowered for marker in SENSITIVE_TEXT):
            raise EvidenceGateError(f"sensitive evidence text is forbidden: {location}")


def _required_string(document: dict, key: str) -> str:
    value = document.get(key)
    if not isinstance(value, str) or not value:
        raise EvidenceGateError(f"evidence field {key} must be a non-empty string")
    return value


def _timestamp(value: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (AttributeError, ValueError) as exc:
        raise EvidenceGateError("observed_at_utc must be an ISO-8601 timestamp") from exc
    if parsed.tzinfo is None:
        raise EvidenceGateError("observed_at_utc must include a timezone")
    parsed = parsed.astimezone(timezone.utc)
    if parsed > datetime.now(timezone.utc):
        raise EvidenceGateError("observed_at_utc must not be in the future")
    return parsed


def validate_observation(document: dict, entry: dict) -> dict:
    if not isinstance(document, dict):
        raise EvidenceGateError("evidence observation must be a JSON object")
    _scan_sensitive(document)
    if document.get("schema_version") != 1 or document.get("status") != "finalized":
        raise EvidenceGateError("evidence observation must be schema v1 with finalized status")
    if document.get("network") != EXPECTED_NETWORK["network"]:
        raise EvidenceGateError("evidence network is not the pinned LEZ testnet")
    identity = document.get("network_identity")
    if not isinstance(identity, dict) or any(
        identity.get(key) != EXPECTED_NETWORK[key]
        for key in ("channel_id", "lez_release", "lez_release_commit")
    ):
        raise EvidenceGateError("evidence network identity does not match the pinned release")
    services = document.get("services")
    if not isinstance(services, dict) or any(
        services.get(key) != EXPECTED_NETWORK[key] for key in ("sequencer", "explorer")
    ):
        raise EvidenceGateError("evidence services do not use the exact official endpoints")

    transaction = _required_string(document, "transaction").lower()
    block_hash = _required_string(document, "block_hash").lower()
    if not HEX_32.fullmatch(transaction) or not HEX_32.fullmatch(block_hash):
        raise EvidenceGateError("transaction and block hash must be lowercase 32-byte hex")
    block = document.get("block")
    if not isinstance(block, int) or isinstance(block, bool) or block < 0:
        raise EvidenceGateError("block must be a non-negative integer")
    transaction_type = document.get("transaction_type")
    if transaction_type != entry["transaction_type"]:
        raise EvidenceGateError("evidence transaction type does not match its inventory row")
    for key in ("kind", "component", "operation"):
        if entry.get(key) is not None and document.get(key) != entry[key]:
            raise EvidenceGateError(f"evidence {key} does not match its inventory row")
    if document.get("finality") != "Finalized":
        raise EvidenceGateError("evidence block is not finalized")
    required_depth = document.get("required_confirmation_depth")
    actual_depth = document.get("confirmation_depth")
    if (
        not isinstance(required_depth, int)
        or required_depth < 3
        or not isinstance(actual_depth, int)
        or actual_depth < required_depth
    ):
        raise EvidenceGateError("evidence confirmation depth is insufficient")
    urls = document.get("explorer_urls")
    expected_urls = {
        "transaction": f"{EXPECTED_NETWORK['explorer']}/transaction/{transaction}",
        "block": f"{EXPECTED_NETWORK['explorer']}/block/{block}",
    }
    if urls != expected_urls:
        raise EvidenceGateError("evidence explorer URLs do not match the exact hash and block")
    checks = document.get("checks")
    if not isinstance(checks, dict) or not checks or not all(value is True for value in checks.values()):
        raise EvidenceGateError("every machine evidence check must pass")
    verifier = document.get("verifier")
    if (
        not isinstance(verifier, dict)
        or verifier.get("result") != "pass"
        or verifier.get("source") != "tools/lez_explorer.py"
        or not isinstance(verifier.get("source_commit"), str)
        or not HEX_40.fullmatch(verifier["source_commit"])
        or not isinstance(verifier.get("observer"), str)
        or not SLUG.fullmatch(verifier["observer"])
    ):
        raise EvidenceGateError("evidence lacks a committed passing verifier")
    observations = document.get("observations")
    tip = observations.get("explorer_latest_finalized") if isinstance(observations, dict) else None
    overlaps = observations.get("overlapping_finalized_blocks") if isinstance(observations, dict) else None
    if (
        not isinstance(tip, dict)
        or not isinstance(tip.get("block_id"), int)
        or tip["block_id"] < block + required_depth
        or not isinstance(overlaps, list)
        or len(overlaps) < 3
        or not all(item.get("matches") is True for item in overlaps if isinstance(item, dict))
        or not all(isinstance(item, dict) for item in overlaps)
    ):
        raise EvidenceGateError("evidence lacks three canonical overlaps and a confirmed tip")
    observed_at = _timestamp(_required_string(document, "observed_at_utc"))
    return {
        "document": document,
        "transaction": transaction,
        "transaction_type": transaction_type,
        "block": block,
        "block_hash": block_hash,
        "observed_at": observed_at,
        "tip": tip["block_id"],
        "required_depth": required_depth,
        "observer": verifier["observer"],
    }


def validate_screenshot(root: Path, sidecar_value: str, observation: dict, page: str) -> dict:
    sidecar_path = evidence_path(root, sidecar_value)
    sidecar = load_json(sidecar_path)
    _scan_sensitive(sidecar)
    if sidecar.get("schema_version") != 1 or sidecar.get("fresh_profile") is not True:
        raise EvidenceGateError("screenshot sidecar must be schema v1 from a fresh browser profile")
    expected_url = observation["document"]["explorer_urls"][page]
    if sidecar.get("url") != expected_url:
        raise EvidenceGateError(f"{page} screenshot URL does not match machine evidence")
    expected_text = sidecar.get("expected_rendered_text")
    if not isinstance(expected_text, list) or not all(isinstance(item, str) for item in expected_text):
        raise EvidenceGateError("screenshot expected_rendered_text is invalid")
    required = [observation["transaction"]]
    if page == "transaction":
        required.append(TRANSACTION_TITLES[observation["transaction_type"]])
    else:
        required.extend(
            [str(observation["block"]), observation["block_hash"], "Finalized"]
        )
    if any(value not in expected_text for value in required):
        raise EvidenceGateError(f"{page} screenshot does not assert all canonical identifiers")
    rendered_digest = sidecar.get("rendered_text_sha256")
    if not isinstance(rendered_digest, str) or not HEX_32.fullmatch(rendered_digest):
        raise EvidenceGateError("screenshot sidecar has an invalid rendered-text SHA-256")
    if not isinstance(sidecar.get("browser"), str) or not sidecar["browser"]:
        raise EvidenceGateError("screenshot sidecar must identify the browser")
    if sidecar.get("navigation_error") is not None:
        raise EvidenceGateError("screenshot browser navigation reported an error")
    screenshot_name = sidecar.get("screenshot")
    if (
        not isinstance(screenshot_name, str)
        or Path(screenshot_name).name != screenshot_name
        or not screenshot_name.endswith(".png")
    ):
        raise EvidenceGateError("screenshot sidecar has an invalid PNG name")
    screenshot = sidecar_path.parent / screenshot_name
    if screenshot.is_symlink() or not screenshot.is_file():
        raise EvidenceGateError(f"screenshot PNG is missing: {screenshot_name}")
    try:
        with screenshot.open("rb") as stream:
            if stream.read(8) != b"\x89PNG\r\n\x1a\n":
                raise EvidenceGateError("screenshot artifact is not a PNG")
    except OSError as exc:
        raise EvidenceGateError(f"could not read screenshot PNG: {exc}") from exc
    if sidecar.get("screenshot_bytes") != screenshot.stat().st_size:
        raise EvidenceGateError("screenshot byte count does not match its PNG")
    digest = sidecar.get("screenshot_sha256")
    if not isinstance(digest, str) or digest != sha256_file(screenshot):
        raise EvidenceGateError("screenshot SHA-256 does not match its PNG")
    dimensions = sidecar.get("dimensions")
    if (
        not isinstance(dimensions, dict)
        or not isinstance(dimensions.get("width"), int)
        or not isinstance(dimensions.get("height"), int)
        or dimensions["width"] < 320
        or dimensions["height"] < 200
    ):
        raise EvidenceGateError("screenshot dimensions are invalid")
    captured_at = _timestamp(_required_string(sidecar, "observed_at_utc"))
    if datetime.now(timezone.utc) - captured_at > MAX_LATEST_OBSERVATION_AGE:
        raise EvidenceGateError("screenshot capture is stale; capture a current browser view")
    return sidecar


def _validate_live(result: dict, expected: dict) -> None:
    for key in ("transaction", "transaction_type", "block", "block_hash"):
        if result.get(key) != expected["document"].get(key):
            raise EvidenceGateError(f"live explorer recheck changed {key}")
    if result.get("status") != "finalized" or not all(result.get("checks", {}).values()):
        raise EvidenceGateError("live explorer recheck did not finalize every check")


def live_reconcile(entry: dict, latest: dict) -> dict:
    explorer = _load_explorer()
    document = latest["document"]
    common = {
        "tx_hash": latest["transaction"],
        "block_id": latest["block"],
        "transaction_type": latest["transaction_type"],
        "verifier_commit": "",
        "observer": "live-release-gate",
        "confirmations": latest["required_depth"],
        "overlap_count": 3,
        "lag_scan": 32,
        "evidence": None,
    }
    try:
        if latest["transaction_type"] == "ProgramDeployment":
            args = Namespace(
                **common,
                bytecode_sha256=document["binary_sha256"],
                bytecode_size=document["binary_size"],
                program_id=document["program_id"],
                release_commit=document["release_commit"],
            )
            result = explorer.reconcile(args)
        else:
            args = Namespace(
                **common,
                kind=entry["kind"],
                component=entry["component"],
                operation=entry["operation"],
                program_id=document.get("public_program_id"),
                account_id=document.get("public_account_ids", []),
            )
            result = explorer.reconcile_transaction(args)
    except explorer.ExplorerValidationError as exc:
        raise VerificationUnavailable(str(exc)) from exc
    _validate_live(result, latest)
    return result


def validate_index(index: dict) -> list[dict]:
    if not isinstance(index, dict) or index.get("schema_version") != 1:
        raise EvidenceGateError("required evidence index must be schema v1")
    if index.get("network") != EXPECTED_NETWORK["network"]:
        raise EvidenceGateError("required evidence index targets the wrong network")
    required = index.get("required_transactions")
    if not isinstance(required, list) or not required:
        raise EvidenceGateError("required evidence index has no transaction rows")
    seen = set()
    for entry in required:
        if not isinstance(entry, dict):
            raise EvidenceGateError("required transaction row must be an object")
        identifier = entry.get("id")
        if not isinstance(identifier, str) or not SLUG.fullmatch(identifier) or identifier in seen:
            raise EvidenceGateError("required transaction IDs must be unique lowercase slugs")
        seen.add(identifier)
        if entry.get("transaction_type") not in TRANSACTION_TITLES:
            raise EvidenceGateError(f"required transaction type is invalid: {identifier}")
        for key in ("kind", "component", "operation"):
            if not isinstance(entry.get(key), str) or not SLUG.fullmatch(entry[key]):
                raise EvidenceGateError(f"required {key} is invalid: {identifier}")
        observations = entry.get("observations")
        screenshots = entry.get("screenshots")
        if not isinstance(observations, list) or len(observations) != 2:
            raise EvidenceGateError(f"exactly two observations are required: {identifier}")
        if len(set(observations)) != 2:
            raise EvidenceGateError(f"observation artifacts must be distinct: {identifier}")
        if not isinstance(screenshots, dict) or set(screenshots) != {"transaction", "block"}:
            raise EvidenceGateError(f"transaction and block screenshots are required: {identifier}")
        if len(set(screenshots.values())) != 2:
            raise EvidenceGateError(f"screenshot sidecars must be distinct: {identifier}")
    return required


def audit(index_path: Path = DEFAULT_INDEX, root: Path = REPO_ROOT, live: bool = False) -> dict:
    root = root.resolve(strict=True)
    index = load_json(index_path.resolve(strict=True))
    _scan_sensitive(index)
    entries = validate_index(index)
    results = []
    transaction_owners = {}
    failures = []
    audited_at = datetime.now(timezone.utc).replace(microsecond=0)
    for entry in entries:
        identifier = entry["id"]
        try:
            observation_paths = [evidence_path(root, value) for value in entry["observations"]]
            observations = [
                validate_observation(load_json(path), entry) for path in observation_paths
            ]
            observations.sort(key=lambda item: item["observed_at"])
            first, latest = observations
            stable = ("transaction", "transaction_type", "block", "block_hash")
            if any(first[key] != latest[key] for key in stable):
                raise EvidenceGateError("independent observations disagree on canonical identifiers")
            if latest["observed_at"] <= first["observed_at"]:
                raise EvidenceGateError("independent observations are not chronologically ordered")
            if latest["observer"] == first["observer"]:
                raise EvidenceGateError("independent observations must name distinct verifier contexts")
            if audited_at - latest["observed_at"] > MAX_LATEST_OBSERVATION_AGE:
                raise EvidenceGateError("latest observation is stale; capture a current audit")
            if latest["tip"] - first["tip"] < latest["required_depth"]:
                raise EvidenceGateError("observations are not separated by confirmation depth")
            owner = transaction_owners.setdefault(latest["transaction"], identifier)
            if owner != identifier:
                raise EvidenceGateError(f"transaction hash is reused by inventory row {owner}")
            transaction_sidecar_path = evidence_path(
                root, entry["screenshots"]["transaction"]
            )
            block_sidecar_path = evidence_path(root, entry["screenshots"]["block"])
            transaction_sidecar = validate_screenshot(
                root, entry["screenshots"]["transaction"], latest, "transaction"
            )
            block_sidecar = validate_screenshot(
                root, entry["screenshots"]["block"], latest, "block"
            )
            live_result = live_reconcile(entry, latest) if live else None
            results.append(
                {
                    "id": identifier,
                    "status": "pass",
                    "transaction": latest["transaction"],
                    "block": latest["block"],
                    "live_recheck": "pass" if live_result is not None else "not-requested",
                    "artifact_sha256": {
                        "observations": [sha256_file(path) for path in observation_paths],
                        "transaction_sidecar": sha256_file(transaction_sidecar_path),
                        "transaction_screenshot": transaction_sidecar["screenshot_sha256"],
                        "block_sidecar": sha256_file(block_sidecar_path),
                        "block_screenshot": block_sidecar["screenshot_sha256"],
                    },
                }
            )
        except VerificationUnavailable as exc:
            failures.append(
                {"id": identifier, "status": "verification-unavailable", "error": str(exc)}
            )
        except (EvidenceGateError, KeyError, OSError, RuntimeError, TypeError, ValueError) as exc:
            failures.append({"id": identifier, "status": "fail", "error": str(exc)})
    status = "pass" if not failures else "fail"
    return {
        "schema_version": 1,
        "verifier_version": VERIFIER_VERSION,
        "status": status,
        "mode": "live-read-only" if live else "offline-artifact-audit",
        "audited_at_utc": audited_at.isoformat(),
        "index_sha256": sha256_file(index_path.resolve(strict=True)),
        "required_count": len(entries),
        "passed_count": len(results),
        "failed_count": len(failures),
        "results": results,
        "failures": failures,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Audit required LEZ explorer evidence")
    result.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    result.add_argument("--live", action="store_true", help="repeat official read-only lookups")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        report = audit(args.index, live=args.live)
        print(json.dumps(report, sort_keys=True))
        return 0 if report["status"] == "pass" else 3
    except (EvidenceGateError, OSError, ValueError) as exc:
        print(json.dumps({"schema_version": 1, "status": "fail", "error": str(exc)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
