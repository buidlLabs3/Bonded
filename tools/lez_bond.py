#!/usr/bin/env python3

"""Typed Bonded lifecycle calls through the pinned official LEZ wallet FFI."""

import argparse
import base64
import contextlib
import ctypes
import hashlib
import importlib.util
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
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
lez_explorer = _load_tool("lez_explorer")

CANONICAL_PROGRAM_ID = "50ce86eebf3a01a5febe8cc735895adf361c2fa43a14947277e3d1050fbdcb8b"
AUTHENTICATED_TRANSFER_PROGRAM_ID = (
    "fe96c4228babbe8bc578e3e25b884cacb07f8c86541f27ed676789875eef875a"
)
STATE_SEED_DOMAIN = b"/Bonded/v1/State/00000000000000/"
ESCROW_SEED_DOMAIN = b"/Bonded/v1/Escrow/0000000000000/"
HEX_32 = re.compile(r"(?i)^[0-9a-f]{64}$")
OUTCOMES = {
    "refund-accepted": 0,
    "sink-rejected": 1,
    "refund-expired": 2,
    "refund-delivery-failed": 3,
}
RESUME_BINDING_FIELDS = (
    "schema_version",
    "operation",
    "outcome",
    "network",
    "network_identity",
    "program_id",
    "binary_sha256",
    "binary_size",
    "bond_id",
    "call",
    "accounts",
    "instruction_word_count",
    "instruction_words_sha256",
    "proof_mode",
    "execution",
    "fee_cu",
    "official_wallet",
)
SECRET_MARKERS = lez_wallet.SENSITIVE_MARKERS + (
    "generated new private",
    "nullifier secret",
    "viewing secret",
    "secret key:",
)


class BondAdapterError(RuntimeError):
    pass


def configure_execution(args) -> dict:
    if args.prover not in ("ipc", "actor"):
        raise BondAdapterError("RISC Zero prover must be an explicit local backend")
    if (
        not isinstance(args.rayon_threads, int)
        or isinstance(args.rayon_threads, bool)
        or args.rayon_threads <= 0
    ):
        raise BondAdapterError("Rayon thread count must be a positive integer")
    os.environ["RISC0_PROVER"] = args.prover
    os.environ["RAYON_NUM_THREADS"] = str(args.rayon_threads)
    return {
        "risc0_prover": args.prover,
        "rayon_num_threads": args.rayon_threads,
    }


class FfiBytes32(ctypes.Structure):
    _fields_ = [("data", ctypes.c_uint8 * 32)]

    @classmethod
    def from_bytes(cls, value: bytes):
        if len(value) != 32:
            raise BondAdapterError("account and digest values must be exactly 32 bytes")
        result = cls()
        result.data[:] = value
        return result

    def as_bytes(self) -> bytes:
        return bytes(self.data)


class FfiProgramId(ctypes.Structure):
    _fields_ = [("data", ctypes.c_uint32 * 8)]


class FfiU128(ctypes.Structure):
    _fields_ = [("data", ctypes.c_uint8 * 16)]


class FfiAccount(ctypes.Structure):
    _fields_ = [
        ("program_owner", FfiProgramId),
        ("balance", FfiU128),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("data_len", ctypes.c_size_t),
        ("nonce", FfiU128),
    ]


class FfiAccountIdentity(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("account_id", FfiBytes32),
        ("key_path", ctypes.c_void_p),
        ("nullifier_secret_key", FfiBytes32),
        ("nullifier_public_key", FfiBytes32),
        ("viewing_public_key", ctypes.POINTER(ctypes.c_uint8)),
        ("viewing_public_key_len", ctypes.c_size_t),
        ("identifier", FfiU128),
    ]


class FfiProgram(ctypes.Structure):
    _fields_ = [
        ("elf_data", ctypes.POINTER(ctypes.c_uint8)),
        ("elf_size", ctypes.c_size_t),
    ]


class FfiProgramWithDependencies(ctypes.Structure):
    _fields_ = [
        ("program", FfiProgram),
        ("deps", ctypes.POINTER(FfiProgram)),
        ("deps_size", ctypes.c_size_t),
    ]


class FfiTransactionResult(ctypes.Structure):
    _fields_ = [
        ("tx_hash", ctypes.c_void_p),
        ("success", ctypes.c_bool),
        ("secrets_data", ctypes.POINTER(FfiBytes32)),
        ("secrets_size", ctypes.c_size_t),
    ]


def program_id_from_hex(value: str) -> FfiProgramId:
    if not HEX_32.fullmatch(value):
        raise BondAdapterError("program ID must be 32-byte hex")
    raw = bytes.fromhex(value)
    return FfiProgramId((ctypes.c_uint32 * 8)(*struct.unpack("<8I", raw)))


def program_id_to_hex(value: FfiProgramId) -> str:
    return b"".join(struct.pack("<I", part) for part in value.data).hex()


def _hex32(value: str, label: str) -> bytes:
    if not HEX_32.fullmatch(value):
        raise BondAdapterError(f"{label} must be 32-byte hex")
    return bytes.fromhex(value)


def initialize_words(args, accounts: dict[str, FfiBytes32]) -> list[int]:
    words = [0]
    for value in (
        _hex32(args.bond_id, "bond ID"),
        _hex32(args.message_commitment, "message commitment"),
        _hex32(args.policy_commitment, "policy commitment"),
        accounts["sender"].as_bytes(),
        accounts["owner"].as_bytes(),
        accounts["sink"].as_bytes(),
    ):
        words.extend(value)
    words.extend(struct.unpack("<4I", args.amount.to_bytes(16, "little")))
    words.extend(struct.unpack("<2I", args.deadline_ms.to_bytes(8, "little")))
    return words


def settle_words(outcome: str) -> list[int]:
    return [1, OUTCOMES[outcome]]


def instruction_digest(words: list[int]) -> str:
    encoded = b"".join(struct.pack("<I", word) for word in words)
    return hashlib.sha256(encoded).hexdigest()


def verify_program(elf: Path, expected_program_id: str) -> dict:
    inspection = lez_wallet.inspect_deployment(elf)
    try:
        process = subprocess.run(
            ["r0vm", "--elf", inspection["binary"], "--id"],
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise BondAdapterError(f"could not derive LEZ program ID: {exc}") from exc
    program_id = process.stdout.strip().lower()
    if program_id != expected_program_id:
        raise BondAdapterError(
            f"loaded Bonded guest ID {program_id}, expected canonical {expected_program_id}"
        )
    return {**inspection, "program_id": program_id}


class OfficialWalletFfi:
    SUCCESS = 0

    def __init__(self, provenance: dict, wallet: dict):
        self.provenance = provenance
        self.wallet = wallet
        self.lib = ctypes.CDLL(provenance["ffi_library"], mode=ctypes.RTLD_LOCAL)
        self._bind()
        self.handle = self.lib.wallet_ffi_open(
            os.fsencode(wallet["config"]),
            os.fsencode(wallet["storage"]),
            os.fsencode(str(Path(wallet["home"]) / "statistics.json")),
        )
        if not self.handle:
            raise BondAdapterError("official wallet FFI could not open the external wallet")
        address = self.lib.wallet_ffi_get_sequencer_addr(self.handle)
        if not address:
            self.close()
            raise BondAdapterError("official wallet FFI omitted its sequencer endpoint")
        try:
            sequencer = ctypes.string_at(address).decode("utf-8")
        finally:
            self.lib.wallet_ffi_free_string(address)
        if sequencer.rstrip("/") != lez_wallet.OFFICIAL_SEQUENCER:
            self.close()
            raise BondAdapterError("official wallet FFI opened a non-pinned sequencer")

    def _bind(self) -> None:
        lib = self.lib
        lib.wallet_ffi_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
        lib.wallet_ffi_open.restype = ctypes.c_void_p
        lib.wallet_ffi_destroy.argtypes = [ctypes.c_void_p]
        lib.wallet_ffi_destroy.restype = None
        lib.wallet_ffi_save.argtypes = [ctypes.c_void_p]
        lib.wallet_ffi_save.restype = ctypes.c_int
        lib.wallet_ffi_get_sequencer_addr.argtypes = [ctypes.c_void_p]
        lib.wallet_ffi_get_sequencer_addr.restype = ctypes.c_void_p
        lib.wallet_ffi_free_string.argtypes = [ctypes.c_void_p]
        lib.wallet_ffi_free_string.restype = None
        lib.wallet_ffi_account_id_from_base58.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(FfiBytes32),
        ]
        lib.wallet_ffi_account_id_from_base58.restype = ctypes.c_int
        lib.wallet_ffi_account_id_to_base58.argtypes = [ctypes.POINTER(FfiBytes32)]
        lib.wallet_ffi_account_id_to_base58.restype = ctypes.c_void_p
        lib.wallet_ffi_account_id_for_public_pda.argtypes = [FfiProgramId, FfiBytes32]
        lib.wallet_ffi_account_id_for_public_pda.restype = FfiBytes32
        lib.wallet_ffi_resolve_public_account.argtypes = [
            FfiBytes32,
            ctypes.c_bool,
            ctypes.POINTER(FfiAccountIdentity),
        ]
        lib.wallet_ffi_resolve_public_account.restype = ctypes.c_int
        lib.wallet_ffi_free_account_identity.argtypes = [ctypes.POINTER(FfiAccountIdentity)]
        lib.wallet_ffi_free_account_identity.restype = None
        lib.wallet_ffi_get_account_public.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(FfiBytes32),
            ctypes.POINTER(FfiAccount),
        ]
        lib.wallet_ffi_get_account_public.restype = ctypes.c_int
        lib.wallet_ffi_free_account_data.argtypes = [ctypes.POINTER(FfiAccount)]
        lib.wallet_ffi_free_account_data.restype = None
        lib.wallet_ffi_transfer_elf.argtypes = [ctypes.POINTER(FfiProgram)]
        lib.wallet_ffi_transfer_elf.restype = ctypes.c_int
        lib.wallet_ffi_free_ffi_program.argtypes = [ctypes.POINTER(FfiProgram)]
        lib.wallet_ffi_free_ffi_program.restype = None
        lib.wallet_ffi_send_generic_private_transaction.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(FfiAccountIdentity),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_size_t,
            ctypes.POINTER(FfiProgramWithDependencies),
            ctypes.POINTER(FfiTransactionResult),
        ]
        lib.wallet_ffi_send_generic_private_transaction.restype = ctypes.c_int
        lib.wallet_ffi_free_transaction_result.argtypes = [
            ctypes.POINTER(FfiTransactionResult)
        ]
        lib.wallet_ffi_free_transaction_result.restype = None

    def _require(self, result: int, operation: str) -> None:
        if result != self.SUCCESS:
            raise BondAdapterError(f"official wallet FFI {operation} failed with code {result}")

    def close(self) -> None:
        if getattr(self, "handle", None):
            self.lib.wallet_ffi_destroy(self.handle)
            self.handle = None

    def account(self, value: str) -> FfiBytes32:
        result = FfiBytes32()
        self._require(
            self.lib.wallet_ffi_account_id_from_base58(
                value.encode("ascii"), ctypes.byref(result)
            ),
            "account decode",
        )
        return result

    def display_account(self, value: FfiBytes32) -> str:
        output = self.lib.wallet_ffi_account_id_to_base58(ctypes.byref(value))
        if not output:
            raise BondAdapterError("official wallet FFI could not encode an account ID")
        try:
            return ctypes.string_at(output).decode("ascii")
        finally:
            self.lib.wallet_ffi_free_string(output)

    def pda(self, program_id: FfiProgramId, domain: bytes, bond_id: bytes) -> FfiBytes32:
        seed = hashlib.sha256(domain + bond_id).digest()
        return self.lib.wallet_ffi_account_id_for_public_pda(
            program_id, FfiBytes32.from_bytes(seed)
        )

    def snapshot(self, account_id: FfiBytes32) -> dict:
        account = FfiAccount()
        self._require(
            self.lib.wallet_ffi_get_account_public(
                self.handle, ctypes.byref(account_id), ctypes.byref(account)
            ),
            "account query",
        )
        try:
            data = (
                ctypes.string_at(account.data, account.data_len)
                if account.data_len
                else b""
            )
            return {
                "account_id": self.display_account(account_id),
                "program_owner": program_id_to_hex(account.program_owner),
                "balance": str(int.from_bytes(bytes(account.balance.data), "little")),
                "nonce": str(int.from_bytes(bytes(account.nonce.data), "little")),
                "data_size": len(data),
                "data_sha256": hashlib.sha256(data).hexdigest(),
            }
        finally:
            self.lib.wallet_ffi_free_account_data(ctypes.byref(account))

    def submit(self, account_specs: list[tuple[FfiBytes32, bool]], words: list[int], elf: Path):
        identities = (FfiAccountIdentity * len(account_specs))()
        for index, (account, sign) in enumerate(account_specs):
            self._require(
                self.lib.wallet_ffi_resolve_public_account(
                    account, sign, ctypes.byref(identities[index])
                ),
                "public account resolution",
            )
        guest = elf.read_bytes()
        guest_buffer = (ctypes.c_uint8 * len(guest)).from_buffer_copy(guest)
        primary = FfiProgram(
            ctypes.cast(guest_buffer, ctypes.POINTER(ctypes.c_uint8)), len(guest)
        )
        transfer = FfiProgram()
        self._require(
            self.lib.wallet_ffi_transfer_elf(ctypes.byref(transfer)),
            "authenticated-transfer dependency load",
        )
        dependencies = (FfiProgram * 1)(transfer)
        program = FfiProgramWithDependencies(
            primary,
            ctypes.cast(dependencies, ctypes.POINTER(FfiProgram)),
            1,
        )
        instruction = (ctypes.c_uint32 * len(words))(*words)
        result = FfiTransactionResult()
        try:
            self._require(
                self.lib.wallet_ffi_send_generic_private_transaction(
                    self.handle,
                    identities,
                    len(identities),
                    instruction,
                    len(instruction),
                    ctypes.byref(program),
                    ctypes.byref(result),
                ),
                "privacy-preserving Bonded call",
            )
            if not result.success or not result.tx_hash:
                raise BondAdapterError("official wallet FFI omitted the submitted transaction hash")
            if result.secrets_size != 0:
                raise BondAdapterError(
                    "public-account Bonded call unexpectedly returned private shared secrets"
                )
            transaction = ctypes.string_at(result.tx_hash).decode("ascii").lower()
            if not HEX_32.fullmatch(transaction):
                raise BondAdapterError("official wallet FFI returned an invalid transaction hash")
            self._require(self.lib.wallet_ffi_save(self.handle), "wallet save")
            return transaction
        finally:
            self.lib.wallet_ffi_free_transaction_result(ctypes.byref(result))
            self.lib.wallet_ffi_free_ffi_program(ctypes.byref(transfer))
            for identity in identities:
                self.lib.wallet_ffi_free_account_identity(ctypes.byref(identity))


@contextlib.contextmanager
def captured_native_output():
    with tempfile.TemporaryDirectory(prefix="bonded-lez-wallet-ffi-") as directory:
        root = Path(directory)
        root.chmod(0o700)
        log = root / "wallet.log"
        captured = {}
        with log.open("w+b") as stream:
            log.chmod(0o600)
            saved_stdout = os.dup(1)
            saved_stderr = os.dup(2)
            try:
                sys.stdout.flush()
                sys.stderr.flush()
                os.dup2(stream.fileno(), 1)
                os.dup2(stream.fileno(), 2)
                yield captured
            finally:
                sys.stdout.flush()
                sys.stderr.flush()
                os.dup2(saved_stdout, 1)
                os.dup2(saved_stderr, 2)
                os.close(saved_stdout)
                os.close(saved_stderr)
                stream.flush()
                captured.update(scan_native_log(log))


def scan_native_log(path: Path) -> dict:
    data = path.read_bytes()
    lowered = data.decode("utf-8", errors="replace").lower()
    if any(marker in lowered for marker in SECRET_MARKERS):
        raise BondAdapterError("official wallet FFI emitted potential secret material")
    return {"captured_output_sha256": hashlib.sha256(data).hexdigest(), "captured_output_bytes": len(data)}


def wait_for_finalized(transaction: str, timeout: float, expected_kind: str = "PrivacyPreserving") -> dict:
    if expected_kind not in lez_explorer.TRANSACTION_VARIANTS.values():
        raise BondAdapterError(f"unsupported transaction kind: {expected_kind}")
    deadline = time.monotonic() + timeout
    last_status = "not-indexed"
    last_rpc_error = None
    while time.monotonic() < deadline:
        try:
            result = lez_explorer.rpc_call("getTransaction", [transaction])
            if isinstance(result, list) and len(result) == 2 and isinstance(result[1], int):
                raw = base64.b64decode(result[0], validate=True)
                kind = lez_explorer.TRANSACTION_VARIANTS.get(raw[0]) if raw else None
                if kind != expected_kind:
                    raise BondAdapterError(
                        f"sequencer returned {kind or 'unknown'} instead of {expected_kind}"
                    )
                computed = hashlib.sha256(raw[1:]).hexdigest()
                if computed != transaction:
                    raise BondAdapterError("sequencer transaction bytes do not match wallet hash")
                block = lez_explorer.decode_sequencer_block(
                    lez_explorer.rpc_call("getBlock", [result[1]])
                )
                last_status = block["bedrock_status"]
                last_rpc_error = None
                if last_status == "Finalized":
                    return {
                        "transaction": transaction,
                        "transaction_type": kind,
                        "serialized_transaction_sha256": computed,
                        "serialized_transaction_bytes": len(raw) - 1,
                        "block": result[1],
                        "block_hash": block["hash"],
                        "finality": last_status,
                    }
        except lez_explorer.ExplorerTransportError as exc:
            last_rpc_error = str(exc).split(":", 1)[0]
        time.sleep(12)
    suffix = f"; last RPC error {last_rpc_error}" if last_rpc_error else ""
    raise BondAdapterError(
        f"transaction did not reach sequencer finality within {timeout:g} seconds; "
        f"last state {last_status}{suffix}"
    )


def _validate_relationships(operation: str, args, before: dict, after: dict) -> None:
    balance = lambda values, key: int(values[key]["balance"])
    if operation == "initialize":
        if balance(after, "sender") != balance(before, "sender") - args.amount:
            raise BondAdapterError("sender balance does not reflect the exact bond lock")
        if balance(before, "escrow") != 0 or balance(after, "escrow") != args.amount:
            raise BondAdapterError("escrow balance does not reflect the exact bond lock")
        if before["state"]["data_size"] != 0 or after["state"]["data_size"] == 0:
            raise BondAdapterError("bond state PDA was not initialized exactly once")
        if after["state"]["program_owner"] != CANONICAL_PROGRAM_ID:
            raise BondAdapterError("bond state PDA has the wrong program owner")
        if after["escrow"]["program_owner"] != AUTHENTICATED_TRANSFER_PROGRAM_ID:
            raise BondAdapterError("bond escrow has the wrong transfer program owner")
    else:
        amount = balance(before, "escrow")
        if amount <= 0 or balance(after, "escrow") != 0:
            raise BondAdapterError("settlement did not drain the bond escrow exactly once")
        if balance(after, "destination") != balance(before, "destination") + amount:
            raise BondAdapterError("settlement destination did not receive the full escrow")
        if balance(after, "owner") != balance(before, "owner"):
            raise BondAdapterError("settlement changed the inbox owner's balance")
        if before["state"]["data_sha256"] == after["state"]["data_sha256"]:
            raise BondAdapterError("settlement did not update terminal bond state")


def _load_candidate(path: Path) -> dict | None:
    if not path.exists():
        return None
    if path.is_symlink() or not path.is_file():
        raise BondAdapterError("lifecycle evidence must be a regular file, not a symlink")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BondAdapterError(f"could not resume lifecycle evidence: {exc}") from exc
    if not isinstance(document, dict):
        raise BondAdapterError("lifecycle evidence must contain a JSON object")
    if document.get("status") not in (
        "submitting",
        "submitted",
        "official-wallet-sequencer-finalized-candidate",
    ):
        raise BondAdapterError("lifecycle evidence has an unsupported status")
    return document


def _validate_resume(candidate: dict, inspection: dict) -> None:
    for field in RESUME_BINDING_FIELDS:
        if candidate.get(field) != inspection.get(field):
            raise BondAdapterError(
                f"lifecycle evidence {field} does not match this exact call"
            )
    state = candidate.get("state")
    if not isinstance(state, dict) or not isinstance(state.get("before"), dict):
        raise BondAdapterError("lifecycle evidence omits the authoritative pre-call state")
    if candidate["status"] in (
        "submitted",
        "official-wallet-sequencer-finalized-candidate",
    ) and not HEX_32.fullmatch(str(candidate.get("transaction", ""))):
        raise BondAdapterError("lifecycle evidence contains an invalid transaction hash")
    if candidate["status"] == "official-wallet-sequencer-finalized-candidate":
        if candidate.get("finality") != "Finalized" or not isinstance(
            candidate.get("state", {}).get("after"), dict
        ):
            raise BondAdapterError("finalized lifecycle evidence is incomplete")


def execute(args) -> dict:
    execution = configure_execution(args)
    profile = lez_wallet.load_network_profile(args.profile.resolve(strict=True))
    provenance = lez_wallet.verify_source(args.wallet_source, profile)
    wallet = lez_wallet.verify_wallet_home(args.wallet_home, profile)
    program = verify_program(args.elf.resolve(strict=True), args.program_id.lower())
    ffi = OfficialWalletFfi(provenance, wallet)
    try:
        accounts = {
            "sender": ffi.account(args.sender),
            "owner": ffi.account(args.owner),
            "sink": ffi.account(args.sink),
        }
        if len({value.as_bytes() for value in accounts.values()}) != 3:
            raise BondAdapterError("sender, owner, and sink must be distinct")
        program_id = program_id_from_hex(program["program_id"])
        bond_id = _hex32(args.bond_id, "bond ID")
        accounts["state"] = ffi.pda(program_id, STATE_SEED_DOMAIN, bond_id)
        accounts["escrow"] = ffi.pda(program_id, ESCROW_SEED_DOMAIN, bond_id)
        if args.operation == "initialize":
            words = initialize_words(args, accounts)
            ordered = [
                (accounts["sender"], True),
                (accounts["state"], False),
                (accounts["escrow"], False),
            ]
            snapshot_names = ("sender", "owner", "sink", "state", "escrow")
            destination = None
        else:
            words = settle_words(args.outcome)
            destination = (
                accounts["sink"] if args.outcome == "sink-rejected" else accounts["sender"]
            )
            accounts["destination"] = destination
            ordered = [
                (accounts["state"], False),
                (accounts["escrow"], False),
                (destination, False),
            ]
            if args.outcome != "refund-expired":
                ordered.append((accounts["owner"], True))
            snapshot_names = ("sender", "owner", "sink", "state", "escrow", "destination")
        inspection = {
            "schema_version": 1,
            "status": "inspection-only",
            "operation": args.operation,
            "outcome": getattr(args, "outcome", None),
            "network": profile["network"],
            "network_identity": {
                "channel_id": profile["channel_id"],
                "lez_release": profile["lez_release"],
                "lez_release_commit": profile["release_commit"],
            },
            "program_id": program["program_id"],
            "binary_sha256": program["binary_sha256"],
            "binary_size": program["binary_size"],
            "bond_id": args.bond_id.lower(),
            "call": (
                {
                    "message_commitment": args.message_commitment.lower(),
                    "policy_commitment": args.policy_commitment.lower(),
                    "amount": args.amount,
                    "deadline_ms": args.deadline_ms,
                }
                if args.operation == "initialize"
                else {"outcome": args.outcome}
            ),
            "accounts": {
                name: ffi.display_account(value)
                for name, value in accounts.items()
                if name != "destination" or destination is not None
            },
            "instruction_word_count": len(words),
            "instruction_words_sha256": instruction_digest(words),
            "proof_mode": "risc0-real-privacy-preserving",
            "execution": execution,
            "fee_cu": "reported-by-sequencer-when-available",
            "official_wallet": {
                "source_commit": provenance["source_commit"],
                "ffi_header_sha256": provenance["ffi_header_sha256"],
                "ffi_library_sha256": provenance["ffi_library_sha256"],
            },
        }
        if not args.submit:
            return inspection
        if os.environ.get("BONDED_LEZ_SUBMIT") != "YES":
            raise BondAdapterError("submission requires BONDED_LEZ_SUBMIT=YES")
        if os.environ.get("RISC0_DEV_MODE") != "0":
            raise BondAdapterError("RISC0_DEV_MODE must be exactly 0")

        evidence = _load_candidate(args.evidence)
        if evidence is not None:
            _validate_resume(evidence, inspection)
            if evidence["status"] == "official-wallet-sequencer-finalized-candidate":
                return evidence
            if evidence["status"] == "submitting":
                raise BondAdapterError(
                    "lifecycle submission was interrupted before its transaction hash was "
                    "journaled; reconcile wallet/sequencer state before any retry"
                )
        else:
            before = {name: ffi.snapshot(accounts[name]) for name in snapshot_names}
            evidence = {
                **inspection,
                "status": "submitting",
                "state": {"before": before},
                "prepared_at_utc": datetime.now(timezone.utc)
                .replace(microsecond=0)
                .isoformat(),
            }
            lez_wallet.atomic_json(args.evidence, evidence)
            with captured_native_output() as captured:
                transaction = ffi.submit(ordered, words, Path(program["binary"]))
            evidence.update(
                {
                    **captured,
                    "status": "submitted",
                    "transaction": transaction,
                    "submitted_at_utc": datetime.now(timezone.utc)
                    .replace(microsecond=0)
                    .isoformat(),
                }
            )
            lez_wallet.atomic_json(args.evidence, evidence)

        before = evidence["state"]["before"]
        transaction = evidence["transaction"]
        inclusion = wait_for_finalized(transaction, args.timeout)
        after = {name: ffi.snapshot(accounts[name]) for name in snapshot_names}
        _validate_relationships(args.operation, args, before, after)
        evidence.update(inclusion)
        evidence["status"] = "official-wallet-sequencer-finalized-candidate"
        evidence["state"]["after"] = after
        evidence["observed_at_utc"] = (
            datetime.now(timezone.utc).replace(microsecond=0).isoformat()
        )
        evidence["verification_boundary"] = (
            "Official-wallet real-proof construction, canonical serialized hash, sequencer "
            "finality, and state reconciliation only; explorer promotion is separate."
        )
        lez_wallet.atomic_json(args.evidence, evidence)
        return evidence
    finally:
        ffi.close()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Official-wallet Bonded lifecycle adapter")
    result.add_argument("--profile", type=Path, default=lez_wallet.DEFAULT_PROFILE)
    result.add_argument("--wallet-source", type=Path, required=True)
    result.add_argument("--wallet-home", type=Path, required=True)
    result.add_argument("--elf", type=Path, default=REPO_ROOT / "build/lez/bonded_inbox.bin")
    result.add_argument("--program-id", default=CANONICAL_PROGRAM_ID)
    result.add_argument("--sender", required=True)
    result.add_argument("--owner", required=True)
    result.add_argument("--sink", required=True)
    result.add_argument("--bond-id", required=True)
    result.add_argument("--submit", action="store_true")
    result.add_argument("--timeout", type=float, default=1800.0)
    result.add_argument("--prover", choices=("ipc", "actor"), default="ipc")
    result.add_argument("--rayon-threads", type=int, default=1)
    commands = result.add_subparsers(dest="operation", required=True)
    initialize = commands.add_parser("initialize")
    initialize.add_argument("--message-commitment", required=True)
    initialize.add_argument("--policy-commitment", required=True)
    initialize.add_argument("--amount", required=True, type=int)
    initialize.add_argument("--deadline-ms", required=True, type=int)
    initialize.add_argument(
        "--evidence",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates/bond-initialize.json",
    )
    settle = commands.add_parser("settle")
    settle.add_argument("--outcome", required=True, choices=tuple(OUTCOMES))
    settle.add_argument(
        "--evidence",
        type=Path,
        default=REPO_ROOT / "evidence/testnet/candidates/bond-settle.json",
    )
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.timeout <= 0:
            raise BondAdapterError("timeout must be positive")
        if args.rayon_threads <= 0:
            raise BondAdapterError("Rayon thread count must be positive")
        if args.operation == "initialize":
            if args.amount <= 0 or not 0 < args.deadline_ms <= 0xFFFFFFFFFFFFFFFF:
                raise BondAdapterError("amount and deadline must be positive and in range")
        response = execute(args)
        print(json.dumps(response, sort_keys=True))
        return 0
    except (
        BondAdapterError,
        lez_wallet.WalletAdapterError,
        lez_explorer.ExplorerValidationError,
        OSError,
        ValueError,
    ) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
