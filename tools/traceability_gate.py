#!/usr/bin/env python3

"""Prevent verified-testnet traceability claims without complete public evidence."""

import argparse
import importlib.util
import json
import re
import shlex
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRACEABILITY = REPO_ROOT / "docs/OPERATIONS.md"
DEFAULT_READINESS = REPO_ROOT / "release/submission-readiness.json"
ROW = re.compile(r"^\|\s*([^|]+?)\s*\|(?:[^|]*\|){3}\s*([^|]+?)\s*\|\s*$")
ALLOWED_STATUSES = {"planned", "implemented", "verified-local", "verified-testnet"}
READINESS_STATUSES = {"open", "verified-local", "verified-testnet", "owner-gated"}
READINESS_SCOPES = {
    "local-adapter",
    "standalone-lez",
    "public-testnet",
    "live-logos-core",
    "ci-real-proof",
    "clean-clone",
    "submission",
}
REQUIRED_CRITERION_FIELDS = {
    "id",
    "requirement",
    "owner",
    "required_scope",
    "status",
    "verification_command",
    "pass_condition",
    "evidence",
    "gap",
}


def _load_evidence_gate():
    spec = importlib.util.spec_from_file_location(
        "lez_evidence_gate_traceability", REPO_ROOT / "tools/lez_evidence_gate.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TraceabilityGateError(RuntimeError):
    pass


def rows(path: Path) -> list[dict]:
    try:
        lines = path.resolve(strict=True).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise TraceabilityGateError(f"could not read traceability matrix: {exc}") from exc
    result = []
    seen = set()
    for line in lines:
        match = ROW.match(line)
        if not match:
            continue
        identifier, status = (value.strip().strip("`") for value in match.groups())
        if identifier in ("ID", "---") or set(identifier) == {"-"}:
            continue
        if status not in ALLOWED_STATUSES:
            raise TraceabilityGateError(
                f"traceability row {identifier} has unsupported status: {status}"
            )
        if identifier in seen:
            raise TraceabilityGateError(f"duplicate traceability row: {identifier}")
        seen.add(identifier)
        result.append({"id": identifier, "status": status})
    if not result:
        raise TraceabilityGateError("traceability matrix contains no requirement rows")
    return result


def readiness(path: Path, root: Path) -> dict:
    try:
        document = json.loads(path.resolve(strict=True).read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise TraceabilityGateError(f"could not read readiness audit: {exc}") from exc
    if document.get("schema_version") != 1 or document.get("overall_status") != "not-ready":
        raise TraceabilityGateError("readiness audit must use schema 1 and remain not-ready")
    criteria = document.get("criteria")
    if not isinstance(criteria, list) or not criteria:
        raise TraceabilityGateError("readiness audit contains no criteria")
    seen = set()
    for criterion in criteria:
        if not isinstance(criterion, dict) or set(criterion) != REQUIRED_CRITERION_FIELDS:
            raise TraceabilityGateError("readiness criterion has missing or unsupported fields")
        identifier = criterion["id"]
        if not isinstance(identifier, str) or not identifier or identifier in seen:
            raise TraceabilityGateError(f"invalid or duplicate readiness criterion: {identifier}")
        seen.add(identifier)
        for field in ("requirement", "owner", "verification_command", "pass_condition", "gap"):
            if not isinstance(criterion[field], str) or not criterion[field].strip():
                raise TraceabilityGateError(f"readiness criterion {identifier} has empty {field}")
        command = shlex.split(criterion["verification_command"])
        command_path = None
        if command and command[0].startswith(("bin/", "scripts/", "tools/")):
            command_path = command[0]
        elif len(command) > 1 and command[0] == "python3" and command[1].startswith("tools/"):
            command_path = command[1]
        if command_path is not None and not (root / command_path).is_file():
            raise TraceabilityGateError(
                f"readiness criterion {identifier} references missing command: {command_path}"
            )
        if criterion["status"] not in READINESS_STATUSES:
            raise TraceabilityGateError(
                f"readiness criterion {identifier} has unsupported status: {criterion['status']}"
            )
        if criterion["required_scope"] not in READINESS_SCOPES:
            raise TraceabilityGateError(
                f"readiness criterion {identifier} has unsupported scope: {criterion['required_scope']}"
            )
        evidence = criterion["evidence"]
        if not isinstance(evidence, list) or any(not isinstance(item, str) for item in evidence):
            raise TraceabilityGateError(f"readiness criterion {identifier} has invalid evidence")
        for item in evidence:
            candidate = (root / item).resolve()
            if root.resolve() not in candidate.parents or not candidate.is_file():
                raise TraceabilityGateError(
                    f"readiness criterion {identifier} references missing evidence: {item}"
                )
        if criterion["status"].startswith("verified-") and not evidence:
            raise TraceabilityGateError(
                f"verified readiness criterion {identifier} must reference evidence"
            )
    return document


def verify_criterion(path: Path, evidence_index: Path, root: Path, identifier: str) -> dict:
    document = readiness(path, root)
    criterion = next(
        (item for item in document["criteria"] if item["id"] == identifier), None
    )
    if criterion is None:
        raise TraceabilityGateError(f"unknown readiness criterion: {identifier}")
    if not criterion["status"].startswith("verified-"):
        raise TraceabilityGateError(
            f"readiness criterion {identifier} is {criterion['status']}: {criterion['gap']}"
        )
    if criterion["status"] == "verified-testnet":
        report = _load_evidence_gate().audit(evidence_index, root)
        if report["status"] != "pass":
            raise TraceabilityGateError(
                f"readiness criterion {identifier} requires the offline LEZ evidence gate"
            )
    return {
        "schema_version": 1,
        "status": "pass",
        "criterion": identifier,
        "criterion_status": criterion["status"],
        "scope": criterion["required_scope"],
        "evidence": criterion["evidence"],
    }


def audit(
    traceability: Path, evidence_index: Path, root: Path, readiness_path: Path | None = None
) -> dict:
    requirements = rows(traceability)
    readiness_document = readiness(readiness_path, root) if readiness_path is not None else None
    claimed = [item["id"] for item in requirements if item["status"] == "verified-testnet"]
    readiness_claimed = []
    if readiness_document is not None:
        readiness_claimed = [
            item["id"]
            for item in readiness_document["criteria"]
            if item["status"] == "verified-testnet"
        ]
    evidence_report = None
    if claimed or readiness_claimed:
        gate = _load_evidence_gate()
        evidence_report = gate.audit(evidence_index, root)
        if evidence_report["status"] != "pass":
            raise TraceabilityGateError(
                "verified-testnet claims require the complete offline LEZ evidence gate to pass"
            )
    return {
        "schema_version": 1,
        "status": "pass",
        "requirement_count": len(requirements),
        "verified_testnet_claims": claimed,
        "readiness_criterion_count": (
            len(readiness_document["criteria"]) if readiness_document is not None else 0
        ),
        "readiness_verified_testnet_claims": readiness_claimed,
        "evidence_gate": "pass" if evidence_report is not None else "not-required",
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Validate public-evidence traceability claims")
    result.add_argument("--traceability", type=Path, default=DEFAULT_TRACEABILITY)
    result.add_argument("--readiness", type=Path, default=DEFAULT_READINESS)
    result.add_argument(
        "--evidence-index",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/required-evidence.json",
    )
    result.add_argument("--criterion", help="Fail unless one readiness criterion is verified")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.criterion:
            print(
                json.dumps(
                    verify_criterion(
                        args.readiness, args.evidence_index, REPO_ROOT, args.criterion
                    ),
                    sort_keys=True,
                )
            )
            return 0
        print(
            json.dumps(
                audit(args.traceability, args.evidence_index, REPO_ROOT, args.readiness),
                sort_keys=True,
            )
        )
        return 0
    except (TraceabilityGateError, OSError, RuntimeError, TypeError, ValueError) as exc:
        print(json.dumps({"schema_version": 1, "status": "fail", "error": str(exc)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
