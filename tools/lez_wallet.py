#!/usr/bin/env python3

"""Fail-closed adapter for the pinned, unmodified official LEZ wallet CLI."""

import argparse
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = REPO_ROOT / "config" / "lez-testnet-network.json"
OFFICIAL_SOURCE_REPOSITORY = "https://github.com/logos-blockchain/logos-execution-zone.git"
OFFICIAL_RELEASE_COMMIT = "47eba256479f6f785acbd138834340703cd03401"
OFFICIAL_SEQUENCER = "https://testnet.lez.logos.co"
OFFICIAL_EXPLORER = "https://explorer.testnet.lez.logos.co"
HEX_32 = re.compile(r"(?i)^[0-9a-f]{64}$")
TRANSACTION_HASH = re.compile(r"(?i)Transaction hash is ([0-9a-f]{64})")
BLOCK_ID = re.compile(r"Transaction is included in block ([0-9]+)")
SENSITIVE_MARKERS = (
    "recovery phrase:",
    "mnemonic phrase:",
    "private key:",
    " nsk:",
    " isk:",
)


class WalletAdapterError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _run(command: list[str], cwd: Path | None = None, timeout: float = 30.0) -> str:
    try:
        process = subprocess.run(
            command,
            cwd=cwd,
            check=True,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        raise WalletAdapterError(f"command failed: {command[0]}: {exc}") from exc
    return process.stdout.strip()


def load_network_profile(path: Path) -> dict:
    try:
        profile = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise WalletAdapterError(f"could not read network profile: {exc}") from exc
    expected = {
        "schema_version": 1,
        "network": "lez-testnet",
        "lez_release": "v0.2.4",
        "release_commit": OFFICIAL_RELEASE_COMMIT,
        "source_repository": OFFICIAL_SOURCE_REPOSITORY,
        "sequencer_url": OFFICIAL_SEQUENCER,
        "explorer_url": OFFICIAL_EXPLORER,
        "indexer_url": None,
        "channel_id": "01" * 32,
        "wallet_package": "wallet",
        "wallet_binary_relative": "target/release/wallet",
        "wallet_ffi_header_relative": "lez/wallet-ffi/wallet_ffi.h",
        "wallet_ffi_library_relative": "target/release/deps/libwallet_ffi.so",
        "risc0_dev_mode": 0,
        "required_confirmation_depth": 3,
    }
    if profile != expected:
        differing = sorted(
            key for key in set(profile) | set(expected) if profile.get(key) != expected.get(key)
        )
        raise WalletAdapterError(
            "network profile does not exactly match pinned LEZ testnet: " + ", ".join(differing)
        )
    return profile


def verify_source(source: Path, profile: dict) -> dict:
    source = source.resolve(strict=True)
    if not (source / ".git").exists():
        raise WalletAdapterError("official wallet source is not a Git checkout")
    head = _run(["git", "rev-parse", "HEAD"], cwd=source)
    if head != profile["release_commit"]:
        raise WalletAdapterError(
            f"official wallet source is at {head}, expected {profile['release_commit']}"
        )
    remote = _run(["git", "remote", "get-url", "origin"], cwd=source)
    accepted_remotes = {
        profile["source_repository"],
        profile["source_repository"].removesuffix(".git"),
        "git@github.com:logos-blockchain/logos-execution-zone.git",
    }
    if remote not in accepted_remotes:
        raise WalletAdapterError("official wallet source origin is not the pinned repository")
    tracked_changes = _run(
        ["git", "status", "--porcelain", "--untracked-files=no"], cwd=source
    )
    if tracked_changes:
        raise WalletAdapterError("official wallet source has tracked modifications")
    binary = (source / profile["wallet_binary_relative"]).resolve(strict=True)
    expected_binary = (source / "target" / "release" / "wallet").resolve()
    if binary != expected_binary or not binary.is_file() or not os.access(binary, os.X_OK):
        raise WalletAdapterError("pinned official wallet release binary is missing or not executable")
    ffi_header = (source / profile["wallet_ffi_header_relative"]).resolve(strict=True)
    expected_header = (source / "lez" / "wallet-ffi" / "wallet_ffi.h").resolve()
    if ffi_header != expected_header or not ffi_header.is_file():
        raise WalletAdapterError("pinned official wallet FFI header is missing")
    ffi_library = (source / profile["wallet_ffi_library_relative"]).resolve(strict=True)
    expected_library = (source / "target" / "release" / "deps" / "libwallet_ffi.so").resolve()
    if ffi_library != expected_library or not ffi_library.is_file():
        raise WalletAdapterError("pinned official wallet FFI library is missing")
    return {
        "source": str(source),
        "source_commit": head,
        "binary": str(binary),
        "binary_size": binary.stat().st_size,
        "binary_sha256": sha256_file(binary),
        "ffi_header": str(ffi_header),
        "ffi_header_size": ffi_header.stat().st_size,
        "ffi_header_sha256": sha256_file(ffi_header),
        "ffi_library": str(ffi_library),
        "ffi_library_size": ffi_library.stat().st_size,
        "ffi_library_sha256": sha256_file(ffi_library),
    }


def verify_wallet_home(wallet_home: Path, profile: dict) -> dict:
    if wallet_home.is_symlink():
        raise WalletAdapterError("wallet home must not be a symlink")
    wallet_home = wallet_home.resolve(strict=True)
    repository = REPO_ROOT.resolve()
    if wallet_home == repository or repository in wallet_home.parents:
        raise WalletAdapterError("wallet home must be outside the repository")
    if stat.S_IMODE(wallet_home.stat().st_mode) & 0o077:
        raise WalletAdapterError("wallet home must not be group/world accessible")
    storage = wallet_home / "storage.json"
    config_path = wallet_home / "wallet_config.json"
    if storage.is_symlink() or config_path.is_symlink():
        raise WalletAdapterError("wallet storage and config must not be symlinks")
    if not storage.is_file():
        raise WalletAdapterError(
            "official wallet storage is missing; initialize it separately in a private terminal"
        )
    if stat.S_IMODE(storage.stat().st_mode) & 0o077:
        raise WalletAdapterError("official wallet storage must not be group/world accessible")
    try:
        wallet_config = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise WalletAdapterError(f"could not read official wallet config: {exc}") from exc
    sequencers = wallet_config.get("sequencers")
    if (
        not isinstance(sequencers, list)
        or len(sequencers) != 1
        or not isinstance(sequencers[0], dict)
        or sequencers[0].get("sequencer_addr") != profile["sequencer_url"]
        or sequencers[0].get("basic_auth") is not None
    ):
        raise WalletAdapterError("official wallet config does not match the pinned testnet endpoint")
    return {"home": str(wallet_home), "storage": str(storage), "config": str(config_path)}


def inspect_deployment(elf_path: Path) -> dict:
    elf_path = elf_path.resolve(strict=True)
    bytecode = elf_path.read_bytes()
    if not bytecode.startswith((b"\x7fELF", b"R0BF")):
        raise WalletAdapterError("program binary is not a supported RISC Zero image")
    if len(bytecode) > 0xFFFFFFFF:
        raise WalletAdapterError("program binary exceeds the LEZ Borsh vector limit")
    message = struct.pack("<I", len(bytecode)) + bytecode
    return {
        "operation": "program-deployment",
        "binary": str(elf_path),
        "binary_size": len(bytecode),
        "binary_sha256": hashlib.sha256(bytecode).hexdigest(),
        "conformance_oracle_transaction_hash": hashlib.sha256(message).hexdigest(),
        "proof_mode": "not-applicable-unsigned-program-deployment",
        "fee_cu": "reported-by-official-wallet-or-sequencer-when-available",
    }


def parse_official_deployment_output(output: str, expected_hash: str) -> dict:
    lowered = output.lower()
    if any(marker in lowered for marker in SENSITIVE_MARKERS):
        raise WalletAdapterError(
            "official wallet emitted recovery/key material; output was suppressed"
        )
    hash_match = TRANSACTION_HASH.search(output)
    block_match = BLOCK_ID.search(output)
    if not hash_match or not block_match:
        raise WalletAdapterError("official wallet output omitted transaction hash or block")
    tx_hash = hash_match.group(1).lower()
    if tx_hash != expected_hash:
        raise WalletAdapterError(
            f"official wallet returned {tx_hash}, conformance oracle expected {expected_hash}"
        )
    return {"transaction": tx_hash, "block": int(block_match.group(1))}


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def deploy_program(args, profile: dict) -> dict:
    if not args.submit or os.environ.get("BONDED_LEZ_SUBMIT") != "YES":
        raise WalletAdapterError(
            "submission requires both --submit and BONDED_LEZ_SUBMIT=YES"
        )
    if os.environ.get("RISC0_DEV_MODE") != "0":
        raise WalletAdapterError("RISC0_DEV_MODE must be exactly 0")
    provenance = verify_source(args.wallet_source, profile)
    wallet = verify_wallet_home(args.wallet_home, profile)
    inspection = inspect_deployment(args.elf)
    environment = os.environ.copy()
    environment["LEE_WALLET_HOME_DIR"] = wallet["home"]
    try:
        process = subprocess.run(
            [provenance["binary"], "deploy-program", inspection["binary"]],
            env=environment,
            input="",
            capture_output=True,
            text=True,
            timeout=args.timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise WalletAdapterError(f"official wallet execution failed: {exc}") from exc
    output = process.stdout + "\n" + process.stderr
    if process.returncode != 0:
        if any(marker in output.lower() for marker in SENSITIVE_MARKERS):
            raise WalletAdapterError("official wallet failed after emitting sensitive output")
        raise WalletAdapterError(
            f"official wallet failed with exit {process.returncode}; output suppressed"
        )
    inclusion = parse_official_deployment_output(
        output, inspection["conformance_oracle_transaction_hash"]
    )
    result = {
        "schema_version": 1,
        "status": "official-wallet-sequencer-included",
        "network": profile["network"],
        "network_identity": {
            "channel_id": profile["channel_id"],
            "lez_release": profile["lez_release"],
            "lez_release_commit": profile["release_commit"],
        },
        "services": {
            "sequencer": profile["sequencer_url"],
            "explorer": profile["explorer_url"],
        },
        "official_wallet": {
            "package": profile["wallet_package"],
            "source_repository": profile["source_repository"],
            "source_commit": provenance["source_commit"],
            "binary_size": provenance["binary_size"],
            "binary_sha256": provenance["binary_sha256"],
            "captured_output_sha256": hashlib.sha256(output.encode("utf-8")).hexdigest(),
        },
        **inspection,
        **inclusion,
        "observed_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "verification_boundary": (
            "Official-wallet construction and sequencer inclusion only; run the explorer "
            "verifier before promoting this candidate."
        ),
    }
    atomic_json(args.evidence, result)
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Pinned official LEZ wallet adapter")
    result.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    commands = result.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect-deployment")
    inspect.add_argument("--elf", type=Path, required=True)
    source = commands.add_parser("check-source")
    source.add_argument("--wallet-source", type=Path, required=True)
    check = commands.add_parser("check-wallet")
    check.add_argument("--wallet-source", type=Path, required=True)
    check.add_argument("--wallet-home", type=Path, required=True)
    deploy = commands.add_parser("deploy-program")
    deploy.add_argument("--elf", type=Path, required=True)
    deploy.add_argument("--wallet-source", type=Path, required=True)
    deploy.add_argument("--wallet-home", type=Path, required=True)
    deploy.add_argument("--submit", action="store_true")
    deploy.add_argument("--timeout", type=float, default=900.0)
    deploy.add_argument(
        "--evidence",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates/official-wallet-program.json",
    )
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        profile = load_network_profile(args.profile.resolve(strict=True))
        if args.command == "inspect-deployment":
            response = {"network_profile": profile, **inspect_deployment(args.elf)}
        elif args.command == "check-source":
            response = {
                "status": "ready",
                "network_profile": profile,
                "provenance": verify_source(args.wallet_source, profile),
            }
        elif args.command == "check-wallet":
            response = {
                "status": "ready",
                "network_profile": profile,
                "provenance": verify_source(args.wallet_source, profile),
                "wallet": verify_wallet_home(args.wallet_home, profile),
            }
        else:
            if args.timeout <= 0:
                raise WalletAdapterError("timeout must be positive")
            response = deploy_program(args, profile)
        print(json.dumps(response, sort_keys=True))
        return 0
    except (WalletAdapterError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
