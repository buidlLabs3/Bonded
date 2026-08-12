#!/usr/bin/env python3

"""Prevent verified-testnet traceability claims without complete public evidence."""

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRACEABILITY = REPO_ROOT / "docs/requirements/traceability.md"
ROW = re.compile(r"^\|\s*([^|]+?)\s*\|(?:[^|]*\|){3}\s*([^|]+?)\s*\|\s*$")
ALLOWED_STATUSES = {"planned", "implemented", "verified-local", "verified-testnet"}


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


def audit(traceability: Path, evidence_index: Path, root: Path) -> dict:
    requirements = rows(traceability)
    claimed = [item["id"] for item in requirements if item["status"] == "verified-testnet"]
    evidence_report = None
    if claimed:
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
        "evidence_gate": "pass" if evidence_report is not None else "not-required",
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Validate public-evidence traceability claims")
    result.add_argument("--traceability", type=Path, default=DEFAULT_TRACEABILITY)
    result.add_argument(
        "--evidence-index",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/required-evidence.json",
    )
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        print(json.dumps(audit(args.traceability, args.evidence_index, REPO_ROOT), sort_keys=True))
        return 0
    except (TraceabilityGateError, OSError, RuntimeError, TypeError, ValueError) as exc:
        print(json.dumps({"schema_version": 1, "status": "fail", "error": str(exc)}, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
