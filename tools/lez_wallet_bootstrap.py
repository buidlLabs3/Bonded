#!/usr/bin/env python3

"""Create a disposable LEZ testnet wallet without printing recovery material."""

import argparse
import ctypes
import importlib.util
import json
import os
import shutil
import stat
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
AGENT_PROFILES = ("inbox", "vault", "settlement")


def _load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lez_wallet = _load_tool("lez_wallet")
lez_bond = _load_tool("lez_bond")


class BootstrapError(RuntimeError):
    pass


class FfiCreateWalletOutput(ctypes.Structure):
    _fields_ = [("wallet", ctypes.c_void_p), ("mnemonic", ctypes.c_void_p)]


def wallet_config(profile: dict) -> dict:
    return {
        "sequencers": [
            {"sequencer_addr": profile["sequencer_url"], "basic_auth": None}
        ],
        "seq_poll_timeout": "12s",
        "seq_tx_poll_max_blocks": 5,
        "seq_poll_max_retries": 5,
        "seq_block_poll_max_amount": 100,
        "multi_sequencer_client_config": {
            "distribution_limit": 1,
            "calibration_limit": 5,
        },
    }


def validate_target(path: Path) -> Path:
    if not path.is_absolute():
        raise BootstrapError("wallet home must be an absolute path")
    target = path.resolve(strict=False)
    repository = REPO_ROOT.resolve()
    if target == repository or repository in target.parents:
        raise BootstrapError("wallet home must be outside the repository")
    if target.exists():
        raise BootstrapError("wallet home already exists; bootstrap never overwrites a wallet")
    parent = target.parent.resolve(strict=True)
    if not parent.is_dir():
        raise BootstrapError("wallet home parent is not a directory")
    return target


def write_private(path: Path, data: bytes) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        os.close(descriptor)
    if stat.S_IMODE(path.stat().st_mode) != 0o600:
        raise BootstrapError(f"private file permissions are not 0600: {path}")


def _bind(lib, private_agent: bool = False) -> None:
    lib.wallet_ffi_create_new.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
    ]
    lib.wallet_ffi_create_new.restype = FfiCreateWalletOutput
    lib.wallet_ffi_create_account_public.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(lez_bond.FfiBytes32),
    ]
    lib.wallet_ffi_create_account_public.restype = ctypes.c_int
    if private_agent:
        if not hasattr(lib, "wallet_ffi_create_account_private"):
            raise BootstrapError("pinned official wallet FFI does not support private accounts")
        lib.wallet_ffi_create_account_private.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(lez_bond.FfiBytes32),
        ]
        lib.wallet_ffi_create_account_private.restype = ctypes.c_int
    lib.wallet_ffi_account_id_to_base58.argtypes = [
        ctypes.POINTER(lez_bond.FfiBytes32)
    ]
    lib.wallet_ffi_account_id_to_base58.restype = ctypes.c_void_p
    lib.wallet_ffi_save.argtypes = [ctypes.c_void_p]
    lib.wallet_ffi_save.restype = ctypes.c_int
    lib.wallet_ffi_free_string.argtypes = [ctypes.c_void_p]
    lib.wallet_ffi_free_string.restype = None
    lib.wallet_ffi_destroy.argtypes = [ctypes.c_void_p]
    lib.wallet_ffi_destroy.restype = None


def _require(code: int, operation: str) -> None:
    if code != 0:
        raise BootstrapError(f"official wallet FFI {operation} failed with code {code}")


def _display_account(lib, value: lez_bond.FfiBytes32) -> str:
    output = lib.wallet_ffi_account_id_to_base58(ctypes.byref(value))
    if not output:
        raise BootstrapError("official wallet FFI could not encode a public account")
    try:
        return ctypes.string_at(output).decode("ascii")
    finally:
        lib.wallet_ffi_free_string(output)


def _agent_account(profile: str, account: lez_bond.FfiBytes32, display: str) -> dict:
    if profile not in AGENT_PROFILES:
        raise BootstrapError("agent profile must be inbox, vault, or settlement")
    return {
        "profile": profile,
        "kind": "private-owned",
        "id_hex": account.as_bytes().hex(),
        "id_base58": display,
    }


def create_wallet(args) -> dict:
    if not args.create or os.environ.get("BONDED_LEZ_BOOTSTRAP") != "YES":
        raise BootstrapError("wallet creation requires --create and BONDED_LEZ_BOOTSTRAP=YES")
    target = validate_target(args.wallet_home)
    profile = lez_wallet.load_network_profile(args.profile.resolve(strict=True))
    provenance = lez_wallet.verify_source(args.wallet_source, profile)
    staging = Path(tempfile.mkdtemp(prefix=f".{target.name}.creating-", dir=target.parent))
    lib = None
    output = FfiCreateWalletOutput()
    mnemonic_owned = False
    completed = False
    try:
        staging.chmod(0o700)
        if stat.S_IMODE(staging.stat().st_mode) != 0o700:
            raise BootstrapError("wallet home permissions are not 0700")
        config = staging / "wallet_config.json"
        storage = staging / "storage.json"
        statistics = staging / "statistics.json"
        recovery = staging / "recovery-phrase.txt"
        agent_profile = getattr(args, "agent_profile", None)
        account_manifest = staging / (
            "agent-account.json" if agent_profile else "public-accounts.json"
        )
        write_private(
            config,
            (json.dumps(wallet_config(profile), indent=2, sort_keys=True) + "\n").encode(
                "utf-8"
            ),
        )
        lib = ctypes.CDLL(provenance["ffi_library"], mode=ctypes.RTLD_LOCAL)
        _bind(lib, private_agent=bool(agent_profile))
        with lez_bond.captured_native_output() as captured:
            output = lib.wallet_ffi_create_new(
                os.fsencode(config),
                os.fsencode(storage),
                os.fsencode(statistics),
                b"disposable-testnet-wallet",
            )
            if not output.wallet or not output.mnemonic:
                raise BootstrapError("official wallet FFI could not create the wallet")
            mnemonic_owned = True
            mnemonic = bytearray(ctypes.string_at(output.mnemonic))
            try:
                if not mnemonic or b"\n" in mnemonic or b"\x00" in mnemonic:
                    raise BootstrapError("official wallet FFI returned invalid recovery material")
                write_private(recovery, bytes(mnemonic) + b"\n")
            finally:
                mnemonic[:] = bytes(len(mnemonic))
            accounts = {}
            agent_account = None
            if agent_profile:
                account = lez_bond.FfiBytes32()
                _require(
                    lib.wallet_ffi_create_account_private(
                        output.wallet, ctypes.byref(account)
                    ),
                    f"{agent_profile} private account creation",
                )
                agent_account = _agent_account(
                    agent_profile, account, _display_account(lib, account)
                )
            else:
                for role in ("sender", "owner", "sink"):
                    account = lez_bond.FfiBytes32()
                    _require(
                        lib.wallet_ffi_create_account_public(
                            output.wallet, ctypes.byref(account)
                        ),
                        f"{role} account creation",
                    )
                    accounts[role] = _display_account(lib, account)
                if len(set(accounts.values())) != 3:
                    raise BootstrapError("official wallet generated duplicate role accounts")
            _require(lib.wallet_ffi_save(output.wallet), "wallet save")

        for path in (config, storage, statistics):
            if path.exists():
                path.chmod(0o600)
        manifest = {
            "schema_version": 1,
            "network": profile["network"],
            "source_commit": provenance["source_commit"],
        }
        if agent_profile:
            manifest["agent"] = agent_account
        else:
            manifest["accounts"] = accounts
        write_private(
            account_manifest,
            (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        )
        lez_wallet.verify_wallet_home(staging, profile)
        staging.rename(target)
        completed = True
        verified = lez_wallet.verify_wallet_home(target, profile)
        recovery = target / recovery.name
        account_manifest = target / account_manifest.name
        result = {
            "status": "created-not-registered-or-funded",
            "wallet": verified,
            "recovery_file": str(recovery),
            "official_wallet": {
                "source_commit": provenance["source_commit"],
                "ffi_library_sha256": provenance["ffi_library_sha256"],
            },
            "captured_native_output": captured,
            "security_boundary": (
                "The pinned v0.2.4 wallet does not encrypt storage at rest; wallet home is "
                "0700 and recovery, storage, config, statistics, and account manifest are 0600."
            ),
        }
        if agent_profile:
            result.update({
                "agent_account_manifest": str(account_manifest),
                "agent": agent_account,
            })
        else:
            result.update({
                "public_account_manifest": str(account_manifest),
                "accounts": accounts,
            })
        return result
    finally:
        if lib is not None and mnemonic_owned:
            lib.wallet_ffi_free_string(output.mnemonic)
        if lib is not None and output.wallet:
            lib.wallet_ffi_destroy(output.wallet)
        if not completed and staging.exists():
            shutil.rmtree(staging)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Secret-safe official LEZ wallet bootstrap")
    result.add_argument("--profile", type=Path, default=lez_wallet.DEFAULT_PROFILE)
    result.add_argument("--wallet-source", type=Path, required=True)
    result.add_argument("--wallet-home", type=Path, required=True)
    result.add_argument("--agent-profile", choices=AGENT_PROFILES)
    result.add_argument("--create", action="store_true")
    return result


def main() -> int:
    try:
        print(json.dumps(create_wallet(parser().parse_args()), sort_keys=True))
        return 0
    except (
        BootstrapError,
        lez_wallet.WalletAdapterError,
        lez_bond.BondAdapterError,
        OSError,
        ValueError,
    ) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
