#!/usr/bin/env python3

"""Provision public test identities or one private agent with the pinned LEZ wallet FFI."""

import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
import stat
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lez_wallet = _load_tool("lez_wallet")
lez_bond = _load_tool("lez_bond")

PINATA_ACCOUNT = "EfQhKQAkX2FJiwNii2WFQsGndjvF1Mzd7RuVe7QdPLw7"
PINATA_PRIZE = 150
ROLES = ("sender", "owner", "sink")
AGENT_PROFILES = ("inbox", "vault", "settlement")


class ProvisionError(RuntimeError):
    pass


class FfiTransferResult(ctypes.Structure):
    _fields_ = [("tx_hash", ctypes.c_void_p), ("success", ctypes.c_bool)]


def load_public_accounts(wallet_home: Path) -> dict[str, str]:
    manifest = wallet_home / "public-accounts.json"
    if manifest.is_symlink() or not manifest.is_file():
        raise ProvisionError("private public-account manifest is missing")
    if stat.S_IMODE(manifest.stat().st_mode) != 0o600:
        raise ProvisionError("public-account manifest permissions must be exactly 0600")
    try:
        document = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProvisionError(f"could not read public-account manifest: {exc}") from exc
    accounts = document.get("accounts")
    if not isinstance(accounts, dict) or set(accounts) != set(ROLES):
        raise ProvisionError("public-account manifest must contain sender, owner, and sink")
    if not all(isinstance(value, str) and value for value in accounts.values()):
        raise ProvisionError("public-account manifest contains an invalid account ID")
    if len(set(accounts.values())) != len(ROLES):
        raise ProvisionError("public-account manifest role accounts must be distinct")
    return accounts


def load_agent_account(wallet_home: Path, expected_profile: str) -> dict:
    manifest = wallet_home / "agent-account.json"
    if manifest.is_symlink() or not manifest.is_file():
        raise ProvisionError("private agent-account manifest is missing")
    if stat.S_IMODE(manifest.stat().st_mode) != 0o600:
        raise ProvisionError("agent-account manifest permissions must be exactly 0600")
    try:
        document = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProvisionError(f"could not read agent-account manifest: {exc}") from exc
    agent = document.get("agent")
    if not isinstance(agent, dict):
        raise ProvisionError("agent-account manifest is invalid")
    if agent.get("profile") != expected_profile or agent.get("kind") != "private-owned":
        raise ProvisionError("agent-account manifest profile or kind does not match")
    account_hex = agent.get("id_hex")
    account_base58 = agent.get("id_base58")
    if not isinstance(account_hex, str) or not lez_bond.HEX_32.fullmatch(account_hex):
        raise ProvisionError("agent account ID must be 32-byte hex")
    expected_base58 = lez_bond.lez_explorer.base58_encode(bytes.fromhex(account_hex))
    if account_base58 != expected_base58:
        raise ProvisionError("agent account hex and base58 IDs do not match")
    return {
        "profile": expected_profile,
        "kind": "private-owned",
        "id_hex": account_hex.lower(),
        "id_base58": account_base58,
    }


def solve_pinata(data: bytes) -> tuple[int, dict]:
    if len(data) != 33 or not 0 <= data[0] <= 32:
        raise ProvisionError("official Piñata account contains an invalid challenge")
    difficulty = data[0]
    seed = data[1:]
    solution = 0
    started = time.monotonic()
    while True:
        digest = hashlib.sha256(seed + solution.to_bytes(16, "little")).digest()
        if digest[:difficulty] == bytes(difficulty):
            return solution, {
                "difficulty_zero_bytes": difficulty,
                "challenge_sha256": hashlib.sha256(data).hexdigest(),
                "solution_sha256": hashlib.sha256(solution.to_bytes(16, "little")).hexdigest(),
                "solve_seconds": round(time.monotonic() - started, 3),
            }
        if solution == 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF:
            raise ProvisionError("Piñata solution space exhausted")
        solution += 1


class ProvisionWallet(lez_bond.OfficialWalletFfi):
    def _bind(self) -> None:
        super()._bind()
        lib = self.lib
        lib.wallet_ffi_register_public_account.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(lez_bond.FfiBytes32),
            ctypes.POINTER(FfiTransferResult),
        ]
        lib.wallet_ffi_register_public_account.restype = ctypes.c_int
        amount = ctypes.c_uint8 * 16
        lib.wallet_ffi_claim_pinata.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(lez_bond.FfiBytes32),
            ctypes.POINTER(lez_bond.FfiBytes32),
            ctypes.POINTER(amount),
            ctypes.POINTER(FfiTransferResult),
        ]
        lib.wallet_ffi_claim_pinata.restype = ctypes.c_int
        private_symbols = (
            "wallet_ffi_get_account_private",
            "wallet_ffi_register_private_account",
            "wallet_ffi_claim_pinata_private_owned_already_initialized",
            "wallet_ffi_sync_to_block",
        )
        self.private_supported = all(hasattr(lib, symbol) for symbol in private_symbols)
        if self.private_supported:
            lib.wallet_ffi_get_account_private.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(lez_bond.FfiBytes32),
                ctypes.POINTER(lez_bond.FfiAccount),
            ]
            lib.wallet_ffi_get_account_private.restype = ctypes.c_int
            lib.wallet_ffi_register_private_account.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(lez_bond.FfiBytes32),
                ctypes.POINTER(FfiTransferResult),
            ]
            lib.wallet_ffi_register_private_account.restype = ctypes.c_int
            lib.wallet_ffi_claim_pinata_private_owned_already_initialized.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(lez_bond.FfiBytes32),
                ctypes.POINTER(lez_bond.FfiBytes32),
                ctypes.POINTER(amount),
                ctypes.c_size_t,
                ctypes.c_void_p,
                ctypes.c_size_t,
                ctypes.POINTER(FfiTransferResult),
            ]
            lib.wallet_ffi_claim_pinata_private_owned_already_initialized.restype = ctypes.c_int
            lib.wallet_ffi_sync_to_block.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
            lib.wallet_ffi_sync_to_block.restype = ctypes.c_int
        lib.wallet_ffi_free_transfer_result.argtypes = [ctypes.POINTER(FfiTransferResult)]
        lib.wallet_ffi_free_transfer_result.restype = None

    def account_data(self, account_id: lez_bond.FfiBytes32) -> bytes:
        account = lez_bond.FfiAccount()
        self._require(
            self.lib.wallet_ffi_get_account_public(
                self.handle, ctypes.byref(account_id), ctypes.byref(account)
            ),
            "account data query",
        )
        try:
            return ctypes.string_at(account.data, account.data_len) if account.data_len else b""
        finally:
            self.lib.wallet_ffi_free_account_data(ctypes.byref(account))

    def _transfer_hash(self, operation) -> str:
        result = FfiTransferResult()
        try:
            self._require(operation(ctypes.byref(result)), "public transaction")
            if not result.success or not result.tx_hash:
                raise ProvisionError("official wallet FFI omitted a public transaction hash")
            transaction = ctypes.string_at(result.tx_hash).decode("ascii").lower()
            if not lez_bond.HEX_32.fullmatch(transaction):
                raise ProvisionError("official wallet FFI returned an invalid transaction hash")
            self._require(self.lib.wallet_ffi_save(self.handle), "wallet save")
            return transaction
        finally:
            self.lib.wallet_ffi_free_transfer_result(ctypes.byref(result))

    def register(self, account: lez_bond.FfiBytes32) -> str:
        return self._transfer_hash(
            lambda result: self.lib.wallet_ffi_register_public_account(
                self.handle, ctypes.byref(account), result
            )
        )

    def claim(self, account: lez_bond.FfiBytes32, solution: int) -> str:
        pinata = self.account(PINATA_ACCOUNT)
        value = (ctypes.c_uint8 * 16).from_buffer_copy(solution.to_bytes(16, "little"))
        return self._transfer_hash(
            lambda result: self.lib.wallet_ffi_claim_pinata(
                self.handle,
                ctypes.byref(pinata),
                ctypes.byref(account),
                ctypes.byref(value),
                result,
            )
        )

    def private_snapshot(self, account_id: lez_bond.FfiBytes32) -> dict:
        if not self.private_supported:
            raise ProvisionError("pinned official wallet FFI does not support private agents")
        account = lez_bond.FfiAccount()
        self._require(
            self.lib.wallet_ffi_get_account_private(
                self.handle, ctypes.byref(account_id), ctypes.byref(account)
            ),
            "private account query",
        )
        try:
            data = ctypes.string_at(account.data, account.data_len) if account.data_len else b""
            return {
                "account_id": account_id.as_bytes().hex(),
                "program_owner": lez_bond.program_id_to_hex(account.program_owner),
                "balance": str(int.from_bytes(bytes(account.balance.data), "little")),
                "nonce": str(int.from_bytes(bytes(account.nonce.data), "little")),
                "data_size": len(data),
                "data_sha256": hashlib.sha256(data).hexdigest(),
            }
        finally:
            self.lib.wallet_ffi_free_account_data(ctypes.byref(account))

    def register_private(self, account: lez_bond.FfiBytes32) -> str:
        if not self.private_supported:
            raise ProvisionError("pinned official wallet FFI does not support private agents")
        return self._transfer_hash(
            lambda result: self.lib.wallet_ffi_register_private_account(
                self.handle, ctypes.byref(account), result
            )
        )

    def sync_private_to_block(self, block: int) -> None:
        if not self.private_supported:
            raise ProvisionError("pinned official wallet FFI does not support private agents")
        self._require(
            self.lib.wallet_ffi_sync_to_block(self.handle, block),
            f"private state sync to block {block}",
        )
        self._require(self.lib.wallet_ffi_save(self.handle), "wallet save")

    def claim_private_initialized(
        self, account: lez_bond.FfiBytes32, solution: int
    ) -> str:
        if not self.private_supported:
            raise ProvisionError("pinned official wallet FFI does not support private agents")
        pinata = self.account(PINATA_ACCOUNT)
        value = (ctypes.c_uint8 * 16).from_buffer_copy(solution.to_bytes(16, "little"))
        return self._transfer_hash(
            lambda result: self.lib.wallet_ffi_claim_pinata_private_owned_already_initialized(
                self.handle,
                ctypes.byref(pinata),
                ctypes.byref(account),
                ctypes.byref(value),
                0,
                None,
                0,
                result,
            )
        )


def _candidate(path: Path) -> dict:
    if not path.exists():
        return {"schema_version": 1, "status": "provisioning-in-progress", "operations": []}
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProvisionError(f"could not resume provisioning evidence: {exc}") from exc
    if document.get("status") not in ("provisioning-in-progress", "provisioned"):
        raise ProvisionError("provisioning evidence has an unsupported status")
    return document


def _operation(evidence: dict, operation_id: str):
    matches = [item for item in evidence["operations"] if item.get("id") == operation_id]
    if len(matches) > 1:
        raise ProvisionError(f"duplicate provisioning journal entry: {operation_id}")
    return matches[0] if matches else None


def execute_agent(args) -> dict:
    if not args.submit or os.environ.get("BONDED_LEZ_SUBMIT") != "YES":
        raise ProvisionError("provisioning requires --submit and BONDED_LEZ_SUBMIT=YES")
    if os.environ.get("RISC0_DEV_MODE") != "0":
        raise ProvisionError("RISC0_DEV_MODE must be exactly 0")
    execution = lez_bond.configure_execution(args)
    profile = lez_wallet.load_network_profile(args.profile.resolve(strict=True))
    provenance = lez_wallet.verify_source(args.wallet_source, profile)
    wallet_home = args.wallet_home.resolve(strict=True)
    wallet = lez_wallet.verify_wallet_home(wallet_home, profile)
    agent = load_agent_account(wallet_home, args.agent_profile)
    evidence = _candidate(args.evidence)
    identity = {
        "network": profile["network"],
        "release_commit": profile["release_commit"],
        "wallet_source_commit": provenance["source_commit"],
        "execution": execution,
        "agent": agent,
    }
    for key, value in identity.items():
        if key in evidence and evidence[key] != value:
            raise ProvisionError(f"provisioning evidence {key} does not match this run")
        evidence[key] = value

    operation_id = f"initialize-agent:{args.agent_profile}"
    journaled = _operation(evidence, operation_id)
    if journaled is not None and journaled.get("status") == "finalized":
        return evidence
    if journaled is not None and journaled.get("status") in (
        "submitting-registration",
        "submitting-claim",
    ):
        raise ProvisionError(
            f"provisioning operation {operation_id} was interrupted during submission; "
            "reconcile wallet/sequencer state before any retry"
        )
    resumable = ("registration-submitted", "registered", "claim-submitted")
    if journaled is not None and journaled.get("status") not in resumable:
        raise ProvisionError(f"provisioning journal entry has invalid status: {operation_id}")

    ffi = ProvisionWallet(provenance, wallet)
    try:
        account = lez_bond.FfiBytes32.from_bytes(bytes.fromhex(agent["id_hex"]))
        if journaled is None:
            before = ffi.private_snapshot(account)
            if before["balance"] != "0":
                raise ProvisionError("agent account is already funded but has no journal")
            journaled = {
                "id": operation_id,
                "status": "submitting-registration",
                "operation": "private-register-then-pinata-fund",
                "agent": agent,
                "state": {"before": before},
                "prepared_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
            }
            evidence["operations"].append(journaled)
            lez_wallet.atomic_json(args.evidence, evidence)
            with lez_bond.captured_native_output() as captured:
                registration_transaction = ffi.register_private(account)
            journaled["registration"] = {
                "status": "submitted",
                "captured_native_output": captured,
                "transaction": registration_transaction,
                "submitted_at_utc": datetime.now(timezone.utc)
                .replace(microsecond=0)
                .isoformat(),
            }
            journaled["status"] = "registration-submitted"
            lez_wallet.atomic_json(args.evidence, evidence)

        if journaled["status"] == "registration-submitted":
            registration = journaled["registration"]
            inclusion = lez_bond.wait_for_finalized(
                registration["transaction"], args.timeout, "PrivacyPreserving"
            )
            ffi.sync_private_to_block(inclusion["block"])
            registered = ffi.private_snapshot(account)
            if registered["program_owner"] != lez_bond.AUTHENTICATED_TRANSFER_PROGRAM_ID:
                raise ProvisionError(
                    "private registration did not assign the authenticated-transfer owner"
                )
            if registered["balance"] != "0":
                raise ProvisionError("private registration produced a non-zero balance")
            registration.update(inclusion)
            registration["state"] = {"after": registered}
            registration["status"] = "finalized"
            registration["observed_at_utc"] = (
                datetime.now(timezone.utc).replace(microsecond=0).isoformat()
            )
            journaled["status"] = "registered"
            lez_wallet.atomic_json(args.evidence, evidence)

        if journaled["status"] == "registered":
            pinata = ffi.account(PINATA_ACCOUNT)
            pinata_before = ffi.snapshot(pinata)
            solution, pow_evidence = solve_pinata(ffi.account_data(pinata))
            journaled["pinata"] = {
                "account": PINATA_ACCOUNT,
                "state": {"before": pinata_before},
                "proof_of_work": pow_evidence,
                "prize": str(PINATA_PRIZE),
            }
            journaled["status"] = "submitting-claim"
            lez_wallet.atomic_json(args.evidence, evidence)
            with lez_bond.captured_native_output() as captured:
                transaction = ffi.claim_private_initialized(account, solution)
            journaled.update({
                "status": "claim-submitted",
                "captured_native_output": captured,
                "transaction": transaction,
                "submitted_at_utc": datetime.now(timezone.utc)
                .replace(microsecond=0)
                .isoformat(),
            })
            lez_wallet.atomic_json(args.evidence, evidence)

        inclusion = lez_bond.wait_for_finalized(
            journaled["transaction"], args.timeout, "PrivacyPreserving"
        )
        ffi.sync_private_to_block(inclusion["block"])
        after = ffi.private_snapshot(account)
        before = journaled["state"]["before"]
        pinata_after = ffi.snapshot(ffi.account(PINATA_ACCOUNT))
        pinata_before = journaled["pinata"]["state"]["before"]
        if after["program_owner"] != lez_bond.AUTHENTICATED_TRANSFER_PROGRAM_ID:
            raise ProvisionError("private claim did not initialize the authenticated-transfer owner")
        if int(after["balance"]) != int(before["balance"]) + PINATA_PRIZE:
            raise ProvisionError("private claim did not credit the exact prize")
        if int(pinata_after["balance"]) != int(pinata_before["balance"]) - PINATA_PRIZE:
            raise ProvisionError("private claim did not debit the exact prize")
        journaled["pinata"]["state"]["after"] = pinata_after
        journaled.update(inclusion)
        journaled["state"]["after"] = after
        journaled["status"] = "finalized"
        journaled["observed_at_utc"] = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
        evidence["status"] = "provisioned"
        evidence["verification_boundary"] = (
            "Official-wallet privacy-preserving submission, sequencer finality, and "
            "authoritative wallet state only; explorer promotion is separate."
        )
        lez_wallet.atomic_json(args.evidence, evidence)
        return evidence
    finally:
        ffi.close()


def execute(args) -> dict:
    if args.operation == "initialize-agent":
        return execute_agent(args)
    if not args.submit or os.environ.get("BONDED_LEZ_SUBMIT") != "YES":
        raise ProvisionError("provisioning requires --submit and BONDED_LEZ_SUBMIT=YES")
    if os.environ.get("RISC0_DEV_MODE") != "0":
        raise ProvisionError("RISC0_DEV_MODE must be exactly 0")
    execution = lez_bond.configure_execution(args)
    profile = lez_wallet.load_network_profile(args.profile.resolve(strict=True))
    provenance = lez_wallet.verify_source(args.wallet_source, profile)
    wallet_home = args.wallet_home.resolve(strict=True)
    wallet = lez_wallet.verify_wallet_home(wallet_home, profile)
    accounts = load_public_accounts(wallet_home)
    evidence = _candidate(args.evidence)
    identity = {
        "network": profile["network"],
        "release_commit": profile["release_commit"],
        "wallet_source_commit": provenance["source_commit"],
        "execution": execution,
        "accounts": accounts,
    }
    for key, value in identity.items():
        if key in evidence and evidence[key] != value:
            raise ProvisionError(f"provisioning evidence {key} does not match this run")
        evidence[key] = value
    operation_id = f"{args.operation}:{args.role}"
    journaled = _operation(evidence, operation_id)
    if journaled is not None and journaled.get("status") == "finalized":
        return evidence
    if journaled is not None and journaled.get("status") == "submitting":
        raise ProvisionError(
            f"provisioning operation {operation_id} was interrupted during submission; "
            "reconcile wallet/sequencer state before any retry"
        )
    if journaled is not None and journaled.get("status") != "submitted":
        raise ProvisionError(f"provisioning journal entry has invalid status: {operation_id}")

    ffi = ProvisionWallet(provenance, wallet)
    try:
        account = ffi.account(accounts[args.role])
        if journaled is None:
            before = ffi.snapshot(account)
            pow_evidence = None
            pinata_before = None
            if args.operation == "register":
                if before["program_owner"] == lez_bond.AUTHENTICATED_TRANSFER_PROGRAM_ID:
                    raise ProvisionError("role account is already registered but has no journal")
            else:
                if before["program_owner"] != lez_bond.AUTHENTICATED_TRANSFER_PROGRAM_ID:
                    raise ProvisionError("role account must be registered before funding")
                pinata = ffi.account(PINATA_ACCOUNT)
                pinata_before = ffi.snapshot(pinata)
                solution, pow_evidence = solve_pinata(ffi.account_data(pinata))
            journaled = {
                "id": operation_id,
                "status": "submitting",
                "operation": args.operation,
                "role": args.role,
                "account": accounts[args.role],
                "state": {"before": before},
                "prepared_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
            }
            if args.operation == "fund":
                journaled["pinata"] = {
                    "account": PINATA_ACCOUNT,
                    "state": {"before": pinata_before},
                    "proof_of_work": pow_evidence,
                    "prize": str(PINATA_PRIZE),
                }
            evidence["operations"].append(journaled)
            lez_wallet.atomic_json(args.evidence, evidence)
            with lez_bond.captured_native_output() as captured:
                if args.operation == "register":
                    transaction = ffi.register(account)
                else:
                    transaction = ffi.claim(account, solution)
            journaled.update(
                {
                    "status": "submitted",
                    "captured_native_output": captured,
                    "transaction": transaction,
                    "submitted_at_utc": datetime.now(timezone.utc)
                    .replace(microsecond=0)
                    .isoformat(),
                }
            )
            lez_wallet.atomic_json(args.evidence, evidence)

        transaction = journaled["transaction"]
        inclusion = lez_bond.wait_for_finalized(transaction, args.timeout, "Public")
        after = ffi.snapshot(account)
        before = journaled["state"]["before"]
        if args.operation == "register":
            if after["program_owner"] != lez_bond.AUTHENTICATED_TRANSFER_PROGRAM_ID:
                raise ProvisionError("registration did not assign the authenticated-transfer owner")
            if after["balance"] != "0" or after["data_size"] != 0:
                raise ProvisionError("registration produced unexpected initial account state")
        else:
            pinata_after = ffi.snapshot(ffi.account(PINATA_ACCOUNT))
            pinata_before = journaled["pinata"]["state"]["before"]
            if int(after["balance"]) != int(before["balance"]) + PINATA_PRIZE:
                raise ProvisionError("Piñata claim did not credit the exact prize")
            if int(pinata_after["balance"]) != int(pinata_before["balance"]) - PINATA_PRIZE:
                raise ProvisionError("Piñata claim did not debit the exact prize")
            journaled["pinata"]["state"]["after"] = pinata_after
        journaled.update(inclusion)
        journaled["state"]["after"] = after
        journaled["status"] = "finalized"
        journaled["observed_at_utc"] = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
        required = {f"register:{role}" for role in ROLES} | {"fund:sender"}
        evidence["status"] = (
            "provisioned" if required <= {
                item["id"] for item in evidence["operations"] if item.get("status") == "finalized"
            }
            else "provisioning-in-progress"
        )
        evidence["verification_boundary"] = (
            "Official-wallet submission, canonical serialized hash, sequencer finality, and "
            "authoritative account state only; explorer promotion is separate."
        )
        lez_wallet.atomic_json(args.evidence, evidence)
        return evidence
    finally:
        ffi.close()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Provision official LEZ testnet identities")
    result.add_argument("--profile", type=Path, default=lez_wallet.DEFAULT_PROFILE)
    result.add_argument("--wallet-source", type=Path, required=True)
    result.add_argument("--wallet-home", type=Path, required=True)
    result.add_argument("--role", choices=ROLES)
    result.add_argument("--agent-profile", choices=AGENT_PROFILES)
    result.add_argument("--submit", action="store_true")
    result.add_argument("--timeout", type=float, default=1800)
    result.add_argument("--prover", choices=("ipc", "actor"), default="ipc")
    result.add_argument("--rayon-threads", type=int, default=2)
    result.add_argument(
        "--evidence",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates/wallet-provisioning.json",
    )
    result.add_argument("operation", choices=("register", "fund", "initialize-agent"))
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        if args.timeout <= 0:
            raise ProvisionError("timeout must be positive")
        if args.operation == "initialize-agent":
            if not args.agent_profile or args.role:
                raise ProvisionError("initialize-agent requires --agent-profile and no --role")
        elif not args.role or args.agent_profile:
            raise ProvisionError("public register/fund requires --role and no --agent-profile")
        if args.operation == "fund" and args.role != "sender":
            raise ProvisionError("only the sender role is funded for the lifecycle matrix")
        print(json.dumps(execute(args), sort_keys=True))
        return 0
    except (
        ProvisionError,
        lez_wallet.WalletAdapterError,
        lez_bond.BondAdapterError,
        OSError,
        ValueError,
    ) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
