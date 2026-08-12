#!/usr/bin/env python3

import argparse
import base64
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit


DEFAULT_ENDPOINT = "https://testnet.lez.logos.co"
LEZ_RELEASE_COMMIT = "47eba256479f6f785acbd138834340703cd03401"
EXPECTED_PROGRAMS = {
    "authenticated_transfer": [
        583309054,
        2344528779,
        3806558405,
        2890696795,
        2257354672,
        3978764116,
        2273929063,
        1518858078,
    ],
    "privacy_preserving_circuit": [
        1334328888,
        3910590567,
        1244219104,
        3671232111,
        3138827701,
        405554639,
        4064616947,
        1864368340,
    ],
}
HEX_32 = re.compile(r"(?i)(?<![0-9a-f])([0-9a-f]{64})(?![0-9a-f])")
HEX_40 = re.compile(r"(?i)^[0-9a-f]{40}$")
PROGRAM_MAGICS = (b"\x7fELF", b"R0BF")


class TestnetError(RuntimeError):
    pass


def deployment_payload(elf: bytes) -> str:
    if not elf.startswith(PROGRAM_MAGICS):
        raise TestnetError("program binary is not a supported RISC Zero image")
    if len(elf) > 0xFFFFFFFF:
        raise TestnetError("program binary exceeds the LEZ Borsh vector limit")
    # LeeTransaction::ProgramDeployment is enum variant 2. Its only field is a
    # ProgramDeploymentTransaction containing Message { bytecode: Vec<u8> }.
    encoded = b"\x02" + struct.pack("<I", len(elf)) + elf
    return base64.b64encode(encoded).decode("ascii")


def deployment_transaction_hash(elf: bytes) -> str:
    # ProgramDeploymentTransaction::hash hashes its own Borsh representation,
    # excluding the outer LeeTransaction enum discriminant.
    encoded = struct.pack("<I", len(elf)) + elf
    return hashlib.sha256(encoded).hexdigest()


def rpc_call(endpoint: str, method: str, params: list, timeout: float = 20.0):
    request_body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params},
        separators=(",", ":"),
    ).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=request_body,
        headers={"content-type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
        raise TestnetError(f"LEZ RPC request failed for {method}: {exc}") from exc
    if payload.get("error") is not None:
        error = payload["error"]
        raise TestnetError(
            f"LEZ RPC {method} failed: {error.get('code')} {error.get('message')}"
        )
    if "result" not in payload:
        raise TestnetError(f"LEZ RPC {method} omitted result")
    return payload["result"]


def require_endpoint(endpoint: str, allow_http: bool) -> None:
    parsed = urlsplit(endpoint)
    if not parsed.hostname or parsed.username is not None or parsed.password is not None:
        raise TestnetError("endpoint must have a host and must not contain credentials")
    if parsed.scheme == "https":
        return
    if allow_http and parsed.scheme == "http":
        return
    raise TestnetError("endpoint must use HTTPS (or pass --allow-http for a local sequencer)")


def preflight(endpoint: str, allow_http: bool) -> dict:
    require_endpoint(endpoint, allow_http)
    rpc_call(endpoint, "checkHealth", [])
    block = rpc_call(endpoint, "getLastBlockId", [])
    programs = rpc_call(endpoint, "getProgramIds", [])
    incompatible = [
        name
        for name, expected in EXPECTED_PROGRAMS.items()
        if programs.get(name) != expected
    ]
    if incompatible:
        raise TestnetError(
            "testnet program IDs do not match pinned LEZ v0.2.4: "
            + ", ".join(sorted(incompatible))
        )
    return {
        "status": "healthy",
        "endpoint": endpoint,
        "last_block": block,
        "lez_release_commit": LEZ_RELEASE_COMMIT,
        "required_programs": {
            name: programs[name] for name in sorted(EXPECTED_PROGRAMS)
        },
    }


def image_id(elf_path: Path, r0vm: str) -> str:
    try:
        process = subprocess.run(
            [r0vm, "--elf", str(elf_path), "--id"],
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        raise TestnetError(f"could not compute RISC Zero image ID: {exc}") from exc
    match = HEX_32.search(process.stdout + "\n" + process.stderr)
    if not match:
        raise TestnetError("r0vm did not return a 32-byte image ID")
    return match.group(1).lower()


def transaction_block(endpoint: str, tx_hash: str):
    result = rpc_call(endpoint, "getTransaction", [tx_hash])
    if result is None:
        return None
    if not isinstance(result, list) or len(result) != 2 or not isinstance(result[1], int):
        raise TestnetError("LEZ getTransaction returned an unexpected response shape")
    return result[1]


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def deploy(args) -> dict:
    if os.environ.get("RISC0_DEV_MODE") != "0":
        raise TestnetError("RISC0_DEV_MODE must be exactly 0")
    if not HEX_40.fullmatch(args.release_commit):
        raise TestnetError("release commit must be a full 40-character Git SHA")
    require_endpoint(args.endpoint, args.allow_http)
    elf_path = args.elf.resolve()
    elf = elf_path.read_bytes()
    payload = deployment_payload(elf)
    expected_hash = deployment_transaction_hash(elf)
    program_id = image_id(elf_path, args.r0vm)

    health = preflight(args.endpoint, args.allow_http)
    block = transaction_block(args.endpoint, expected_hash)
    submitted = False
    if block is None:
        returned_hash = rpc_call(args.endpoint, "sendTransaction", [payload])
        if returned_hash != expected_hash:
            raise TestnetError(
                f"sequencer returned transaction hash {returned_hash}, expected {expected_hash}"
            )
        submitted = True
        for _ in range(args.poll_attempts):
            time.sleep(args.poll_interval)
            block = transaction_block(args.endpoint, expected_hash)
            if block is not None:
                break
    if block is None:
        raise TestnetError("deployment transaction was not included before the poll deadline")

    evidence = {
        "status": "verified",
        "kind": "program-deployment",
        "component": "settlement-program",
        "network": "lez-testnet",
        "lez_release": "v0.2.4",
        "endpoint": args.endpoint,
        "program_id": program_id,
        "transaction": expected_hash,
        "block": block,
        "binary_sha256": hashlib.sha256(elf).hexdigest(),
        "binary_size": len(elf),
        "release_commit": args.release_commit,
        "lez_release_commit": LEZ_RELEASE_COMMIT,
        "risc0_dev_mode": 0,
        "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "submitted": submitted,
        "reproduce": "RISC0_DEV_MODE=0 scripts/deploy-lez-testnet.sh",
        "last_block_at_preflight": health["last_block"],
    }
    atomic_json(args.evidence, evidence)
    return evidence


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Fail-closed LEZ testnet deployment client")
    result.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    result.add_argument("--allow-http", action="store_true")
    commands = result.add_subparsers(dest="command", required=True)
    commands.add_parser("preflight")
    status = commands.add_parser("status")
    status.add_argument("--tx-hash", required=True)
    deploy_parser = commands.add_parser("deploy")
    deploy_parser.add_argument("--elf", type=Path, required=True)
    deploy_parser.add_argument("--release-commit", required=True)
    deploy_parser.add_argument(
        "--evidence", type=Path, default=Path("evidence/testnet/settlement-program.json")
    )
    deploy_parser.add_argument("--r0vm", default="r0vm")
    deploy_parser.add_argument("--poll-attempts", type=int, default=60)
    deploy_parser.add_argument("--poll-interval", type=float, default=5.0)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "preflight":
            result = preflight(args.endpoint, args.allow_http)
        elif args.command == "status":
            if not HEX_32.fullmatch(args.tx_hash):
                raise TestnetError("transaction hash must be 32-byte lowercase or uppercase hex")
            require_endpoint(args.endpoint, args.allow_http)
            block = transaction_block(args.endpoint, args.tx_hash)
            result = {
                "status": "included" if block is not None else "pending",
                "transaction": args.tx_hash,
                "block": block,
            }
        else:
            if args.poll_attempts < 1 or args.poll_interval < 0:
                raise TestnetError("poll attempts must be positive and interval non-negative")
            result = deploy(args)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (TestnetError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
