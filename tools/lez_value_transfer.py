#!/usr/bin/env python3

"""Submit application-authorized value transfers through the official LEZ wallet."""

import argparse
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
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lez_bond = _load_tool("lez_bond")
lez_wallet = lez_bond.lez_wallet
AUTHENTICATED_TRANSFER_PROGRAM_ID = lez_bond.AUTHENTICATED_TRANSFER_PROGRAM_ID
HEX_32 = re.compile(r"(?i)^[0-9a-f]{64}$")
OPERATIONS = {
    "below-limit-transfer": {
        "component": "spending-controller",
        "kind": "spending-control",
        "required_roles": {"policy-owner"},
    },
    "owner-approved-transfer": {
        "component": "spending-controller",
        "kind": "spending-control",
        "required_roles": {"owner"},
    },
    "paid-task-settlement": {
        "component": "a2a-service",
        "kind": "paid-task",
        "required_roles": {"requester", "provider"},
    },
}
RESUME_FIELDS = (
    "schema_version",
    "operation",
    "network",
    "network_identity",
    "profile",
    "program_id",
    "program_binary_sha256",
    "program_binary_size",
    "accounts",
    "amount",
    "instruction_word_count",
    "instruction_words_sha256",
    "authorization_sha256",
    "trusted_signers_sha256",
    "proof_mode",
    "official_wallet",
)


class ValueTransferError(RuntimeError):
    pass


ED25519_PRIVATE_DER_PREFIX = bytes.fromhex("302e020100300506032b657004220420")
ED25519_PUBLIC_DER_PREFIX = bytes.fromhex("302a300506032b6570032100")


def _json_object(path: Path, label: str) -> dict:
    if path.is_symlink() or not path.is_file():
        raise ValueTransferError(f"{label} must be a regular non-symlink file")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueTransferError(f"could not read {label}: {exc}") from exc
    if not isinstance(document, dict):
        raise ValueTransferError(f"{label} must contain a JSON object")
    return document


def canonical_payload(payload: dict) -> bytes:
    return json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")


def payload_digest(payload: dict) -> str:
    return hashlib.sha256(canonical_payload(payload)).hexdigest()


def _openssl(args: list[str], operation: str) -> None:
    try:
        subprocess.run(
            ["openssl", *args], check=True, capture_output=True, timeout=30
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ValueTransferError(f"OpenSSL Ed25519 {operation} failed") from exc


def public_key_from_private(private_key: bytes) -> bytes:
    if len(private_key) != 32:
        raise ValueTransferError("Ed25519 private key must be exactly 32 bytes")
    with tempfile.TemporaryDirectory(prefix="bonded-authorization-key-") as directory:
        root = Path(directory)
        private_der = root / "private.der"
        public_der = root / "public.der"
        private_der.write_bytes(ED25519_PRIVATE_DER_PREFIX + private_key)
        private_der.chmod(0o600)
        _openssl(
            [
                "pkey",
                "-in",
                str(private_der),
                "-inform",
                "DER",
                "-pubout",
                "-outform",
                "DER",
                "-out",
                str(public_der),
            ],
            "public-key derivation",
        )
        encoded = public_der.read_bytes()
    if not encoded.startswith(ED25519_PUBLIC_DER_PREFIX) or len(encoded) != 44:
        raise ValueTransferError("OpenSSL returned an invalid Ed25519 public key")
    return encoded[len(ED25519_PUBLIC_DER_PREFIX) :]


def generate_signing_key(path: Path) -> dict:
    if path.is_symlink() or path.exists():
        raise ValueTransferError("signing key path must not already exist")
    try:
        parent = path.parent.resolve(strict=True)
    except OSError as exc:
        raise ValueTransferError("signing key parent directory does not exist") from exc
    if not parent.is_dir() or parent.stat().st_mode & 0o077:
        raise ValueTransferError("signing key parent directory must be private")
    destination = parent / path.name
    key = os.urandom(32)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(destination, flags, 0o600)
        with os.fdopen(descriptor, "w", encoding="ascii") as output:
            output.write(key.hex() + "\n")
            output.flush()
            os.fsync(output.fileno())
        if destination.stat().st_mode & 0o077:
            raise ValueTransferError("generated signing key permissions are not private")
    except OSError as exc:
        raise ValueTransferError("could not create the signing key securely") from exc
    return {
        "algorithm": "Ed25519",
        "private_key_path": str(destination),
        "public_key": public_key_from_private(key).hex(),
    }


def sign_message(private_key: bytes, message: bytes) -> bytes:
    if len(private_key) != 32:
        raise ValueTransferError("Ed25519 private key must be exactly 32 bytes")
    with tempfile.TemporaryDirectory(prefix="bonded-authorization-sign-") as directory:
        root = Path(directory)
        private_der = root / "private.der"
        message_path = root / "message"
        signature_path = root / "signature"
        private_der.write_bytes(ED25519_PRIVATE_DER_PREFIX + private_key)
        private_der.chmod(0o600)
        message_path.write_bytes(message)
        _openssl(
            [
                "pkeyutl",
                "-sign",
                "-rawin",
                "-inkey",
                str(private_der),
                "-keyform",
                "DER",
                "-in",
                str(message_path),
                "-out",
                str(signature_path),
            ],
            "signing",
        )
        signature = signature_path.read_bytes()
    if len(signature) != 64:
        raise ValueTransferError("OpenSSL returned an invalid Ed25519 signature")
    return signature


def verify_signature(public_key: bytes, message: bytes, signature: bytes) -> bool:
    if len(public_key) != 32 or len(signature) != 64:
        return False
    with tempfile.TemporaryDirectory(prefix="bonded-authorization-verify-") as directory:
        root = Path(directory)
        public_der = root / "public.der"
        message_path = root / "message"
        signature_path = root / "signature"
        public_der.write_bytes(ED25519_PUBLIC_DER_PREFIX + public_key)
        message_path.write_bytes(message)
        signature_path.write_bytes(signature)
        try:
            subprocess.run(
                [
                    "openssl",
                    "pkeyutl",
                    "-verify",
                    "-rawin",
                    "-pubin",
                    "-inkey",
                    str(public_der),
                    "-keyform",
                    "DER",
                    "-in",
                    str(message_path),
                    "-sigfile",
                    str(signature_path),
                ],
                check=True,
                capture_output=True,
                timeout=30,
            )
            return True
        except subprocess.CalledProcessError:
            return False
        except (OSError, subprocess.SubprocessError) as exc:
            raise ValueTransferError("OpenSSL Ed25519 verification failed") from exc


def transfer_words(amount: int) -> list[int]:
    if not 0 < amount <= 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF:
        raise ValueTransferError("transfer amount must be positive and fit u128")
    return [0, *struct.unpack("<4I", amount.to_bytes(16, "little"))]


def inspect_transfer_program(guest: bytes) -> dict:
    if not guest:
        raise ValueTransferError("official wallet returned an empty transfer program")
    with tempfile.TemporaryDirectory(prefix="bonded-transfer-program-") as directory:
        path = Path(directory) / "authenticated_transfer.bin"
        path.write_bytes(guest)
        try:
            process = subprocess.run(
                ["r0vm", "--elf", str(path), "--id"],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise ValueTransferError("could not derive authenticated-transfer program ID") from exc
    program_id = process.stdout.strip().lower()
    if program_id != AUTHENTICATED_TRANSFER_PROGRAM_ID:
        raise ValueTransferError(
            f"official wallet transfer program ID {program_id} does not match LEZ canonical ID"
        )
    return {
        "program_id": program_id,
        "program_binary_sha256": hashlib.sha256(guest).hexdigest(),
        "program_binary_size": len(guest),
    }


def instruction_digest(words: list[int]) -> str:
    return hashlib.sha256(b"".join(struct.pack("<I", word) for word in words)).hexdigest()


def _validate_claims(operation: str, payload: dict) -> None:
    claims = payload.get("claims")
    if not isinstance(claims, dict):
        raise ValueTransferError("authorization claims must be an object")
    amount = payload["amount"]
    if operation == "below-limit-transfer":
        policy = claims.get("policy")
        if (
            claims.get("decision") != "autonomous-below-limit"
            or not isinstance(policy, dict)
            or not isinstance(policy.get("per_transaction"), int)
            or not isinstance(policy.get("per_period"), int)
            or isinstance(policy["per_transaction"], bool)
            or isinstance(policy["per_period"], bool)
            or policy["per_transaction"] <= 0
            or policy["per_period"] <= 0
            or amount > policy["per_transaction"]
            or amount > policy["per_period"]
        ):
            raise ValueTransferError("below-limit authorization conflicts with its policy")
    elif operation == "owner-approved-transfer":
        policy = claims.get("policy")
        if (
            claims.get("decision") != "owner-approved"
            or not isinstance(claims.get("proposal_id"), str)
            or not claims["proposal_id"]
            or not isinstance(policy, dict)
            or not isinstance(policy.get("per_transaction"), int)
            or isinstance(policy["per_transaction"], bool)
            or policy["per_transaction"] <= 0
            or amount <= policy["per_transaction"]
        ):
            raise ValueTransferError("owner approval does not prove an above-limit proposal")
    else:
        required = ("task_id", "requester", "provider", "skill")
        if claims.get("task_state") != "completed" or any(
            not isinstance(claims.get(field), str) or not claims[field] for field in required
        ):
            raise ValueTransferError("paid-task authorization is incomplete")


def validate_authorization(
    document: dict,
    now_ms: int | None = None,
    trusted_signers: dict[str, str] | None = None,
) -> dict:
    payload = document.get("payload")
    attestations = document.get("attestations")
    if document.get("schema_version") != 1 or not isinstance(payload, dict):
        raise ValueTransferError("authorization must use schema version 1")
    if document.get("payload_sha256") != payload_digest(payload):
        raise ValueTransferError("authorization payload digest mismatch")
    operation = payload.get("operation")
    if operation not in OPERATIONS:
        raise ValueTransferError("authorization operation is unsupported")
    required = {
        "network": "lez-testnet",
        "sender": str,
        "recipient": str,
        "profile": str,
        "nonce": str,
        "created_at_ms": int,
        "expires_at_ms": int,
        "amount": int,
    }
    for field, expected in required.items():
        value = payload.get(field)
        if expected == "lez-testnet":
            if value != expected:
                raise ValueTransferError("authorization targets the wrong network")
        elif not isinstance(value, expected) or isinstance(value, bool):
            raise ValueTransferError(f"authorization {field} has the wrong type")
    if not payload["sender"] or not payload["recipient"] or payload["sender"] == payload["recipient"]:
        raise ValueTransferError("authorization sender and recipient must be distinct")
    if not payload["profile"] or not HEX_32.fullmatch(payload["nonce"]):
        raise ValueTransferError("authorization profile or nonce is invalid")
    if not 0 < payload["amount"] <= 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF:
        raise ValueTransferError("authorization amount is invalid")
    if payload["created_at_ms"] >= payload["expires_at_ms"]:
        raise ValueTransferError("authorization validity interval is invalid")
    if now_ms is not None and not payload["created_at_ms"] <= now_ms <= payload["expires_at_ms"]:
        raise ValueTransferError("authorization is not currently valid")
    _validate_claims(operation, payload)
    if not isinstance(attestations, list):
        raise ValueTransferError("authorization attestations must be an array")
    message = canonical_payload(payload)
    roles = set()
    for attestation in attestations:
        if not isinstance(attestation, dict):
            raise ValueTransferError("authorization attestation must be an object")
        role = attestation.get("role")
        public_key = attestation.get("public_key")
        signature = attestation.get("signature")
        if role in roles or not isinstance(role, str):
            raise ValueTransferError("authorization attestation roles must be unique")
        if not HEX_32.fullmatch(str(public_key)) or not re.fullmatch(
            r"(?i)[0-9a-f]{128}", str(signature)
        ):
            raise ValueTransferError("authorization attestation key or signature is invalid")
        if not verify_signature(bytes.fromhex(public_key), message, bytes.fromhex(signature)):
            raise ValueTransferError("authorization signature verification failed")
        roles.add(role)
    if roles != OPERATIONS[operation]["required_roles"]:
        raise ValueTransferError("authorization does not contain the exact required signer roles")
    if trusted_signers is not None:
        if set(trusted_signers) != roles or any(
            trusted_signers[item["role"]].lower() != item["public_key"].lower()
            for item in attestations
        ):
            raise ValueTransferError("authorization signer is not pinned by the trusted signer map")
    return payload


def load_trusted_signers(path: Path) -> dict[str, str]:
    document = _json_object(path, "trusted signer map")
    if not all(
        isinstance(role, str) and HEX_32.fullmatch(str(public_key))
        for role, public_key in document.items()
    ):
        raise ValueTransferError("trusted signer map must contain role-to-Ed25519-key entries")
    return {role: public_key.lower() for role, public_key in document.items()}


def create_authorization(args) -> dict:
    claims = _json_object(args.claims, "authorization claims")
    payload = {
        "operation": args.operation,
        "network": "lez-testnet",
        "profile": args.profile,
        "sender": args.sender,
        "recipient": args.recipient,
        "amount": args.amount,
        "nonce": args.nonce.lower(),
        "created_at_ms": args.created_at_ms,
        "expires_at_ms": args.expires_at_ms,
        "claims": claims,
    }
    document = {
        "schema_version": 1,
        "payload": payload,
        "payload_sha256": payload_digest(payload),
        "attestations": [],
    }
    _validate_claims(args.operation, payload)
    lez_wallet.atomic_json(args.evidence, document)
    return document


def sign_authorization(args) -> dict:
    document = _json_object(args.authorization, "authorization")
    payload = document.get("payload")
    if not isinstance(payload, dict) or document.get("payload_sha256") != payload_digest(payload):
        raise ValueTransferError("authorization payload digest mismatch")
    if args.private_key.is_symlink():
        raise ValueTransferError("signing key must be a private regular non-symlink file")
    key_path = args.private_key.resolve(strict=True)
    if not key_path.is_file() or key_path.stat().st_mode & 0o077:
        raise ValueTransferError("signing key must be a private regular non-symlink file")
    try:
        key_bytes = bytes.fromhex(key_path.read_text(encoding="ascii").strip())
    except (OSError, UnicodeError, ValueError) as exc:
        raise ValueTransferError("could not load the raw 32-byte Ed25519 signing key") from exc
    public_key = public_key_from_private(key_bytes).hex()
    attestation = {
        "role": args.role,
        "public_key": public_key,
        "signature": sign_message(key_bytes, canonical_payload(payload)).hex(),
    }
    output = {**document, "attestations": [*document.get("attestations", []), attestation]}
    if len({item.get("role") for item in output["attestations"]}) != len(output["attestations"]):
        raise ValueTransferError("authorization already contains this signer role")
    if {item["role"] for item in output["attestations"]} == OPERATIONS[payload["operation"]][
        "required_roles"
    ]:
        validate_authorization(output)
    lez_wallet.atomic_json(args.evidence, output)
    return output


class OfficialTransferFfi(lez_bond.OfficialWalletFfi):
    def transfer_program(self) -> bytes:
        program = lez_bond.FfiProgram()
        self._require(
            self.lib.wallet_ffi_transfer_elf(ctypes.byref(program)),
            "authenticated-transfer program load",
        )
        try:
            return ctypes.string_at(program.elf_data, program.elf_size)
        finally:
            self.lib.wallet_ffi_free_ffi_program(ctypes.byref(program))

    def submit_transfer(self, sender, recipient, words: list[int], guest: bytes) -> str:
        account_specs = ((sender, True), (recipient, False))
        identities = (lez_bond.FfiAccountIdentity * len(account_specs))()
        for index, (account, sign) in enumerate(account_specs):
            self._require(
                self.lib.wallet_ffi_resolve_public_account(
                    account, sign, ctypes.byref(identities[index])
                ),
                "public account resolution",
            )
        guest_buffer = (ctypes.c_uint8 * len(guest)).from_buffer_copy(guest)
        primary = lez_bond.FfiProgram(
            ctypes.cast(guest_buffer, ctypes.POINTER(ctypes.c_uint8)), len(guest)
        )
        program = lez_bond.FfiProgramWithDependencies(
            primary, ctypes.POINTER(lez_bond.FfiProgram)(), 0
        )
        instruction = (ctypes.c_uint32 * len(words))(*words)
        result = lez_bond.FfiTransactionResult()
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
                "privacy-preserving authenticated transfer",
            )
            if not result.success or not result.tx_hash or result.secrets_size != 0:
                raise ValueTransferError("official wallet returned an incomplete transfer result")
            transaction = ctypes.string_at(result.tx_hash).decode("ascii").lower()
            if not HEX_32.fullmatch(transaction):
                raise ValueTransferError("official wallet returned an invalid transaction hash")
            self._require(self.lib.wallet_ffi_save(self.handle), "wallet save")
            return transaction
        finally:
            self.lib.wallet_ffi_free_transaction_result(ctypes.byref(result))
            for identity in identities:
                self.lib.wallet_ffi_free_account_identity(ctypes.byref(identity))


def _validate_resume(candidate: dict, inspection: dict) -> None:
    for field in RESUME_FIELDS:
        if candidate.get(field) != inspection.get(field):
            raise ValueTransferError(f"transfer journal {field} does not match this call")
    if candidate.get("status") not in (
        "submitting",
        "submitted",
        "official-wallet-sequencer-finalized-candidate",
    ):
        raise ValueTransferError("transfer journal has an unsupported status")


def execute(args) -> dict:
    authorization = _json_object(args.authorization, "authorization")
    now_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
    trusted_signers = load_trusted_signers(args.trusted_signers)
    payload = validate_authorization(authorization, now_ms, trusted_signers)
    for field in ("operation", "profile", "sender", "recipient", "amount"):
        if getattr(args, field) != payload[field]:
            raise ValueTransferError(f"command {field} conflicts with signed authorization")
    profile = lez_wallet.load_network_profile(args.network_profile.resolve(strict=True))
    provenance = lez_wallet.verify_source(args.wallet_source, profile)
    wallet = lez_wallet.verify_wallet_home(args.wallet_home, profile)
    ffi = OfficialTransferFfi(provenance, wallet)
    try:
        sender = ffi.account(args.sender)
        recipient = ffi.account(args.recipient)
        if sender.as_bytes() == recipient.as_bytes():
            raise ValueTransferError("sender and recipient must be distinct")
        guest = ffi.transfer_program()
        program = inspect_transfer_program(guest)
        words = transfer_words(args.amount)
        inspection = {
            "schema_version": 1,
            "status": "inspection-only",
            "operation": args.operation,
            "network": profile["network"],
            "network_identity": {
                "channel_id": profile["channel_id"],
                "lez_release": profile["lez_release"],
                "lez_release_commit": profile["release_commit"],
            },
            "profile": args.profile,
            **program,
            "accounts": {"sender": args.sender, "recipient": args.recipient},
            "amount": args.amount,
            "instruction_word_count": len(words),
            "instruction_words_sha256": instruction_digest(words),
            "authorization_sha256": hashlib.sha256(args.authorization.read_bytes()).hexdigest(),
            "trusted_signers_sha256": hashlib.sha256(
                args.trusted_signers.read_bytes()
            ).hexdigest(),
            "authorization": {
                "payload_sha256": authorization["payload_sha256"],
                "signers": [
                    {"role": item["role"], "public_key": item["public_key"]}
                    for item in authorization["attestations"]
                ],
                "claims": payload["claims"],
                "nonce": payload["nonce"],
            },
            "proof_mode": "risc0-real-privacy-preserving",
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
            raise ValueTransferError("submission requires BONDED_LEZ_SUBMIT=YES")
        if os.environ.get("RISC0_DEV_MODE") != "0":
            raise ValueTransferError("RISC0_DEV_MODE must be exactly 0")
        if args.evidence.exists():
            evidence = _json_object(args.evidence, "transfer journal")
            _validate_resume(evidence, inspection)
            if evidence["status"] == "official-wallet-sequencer-finalized-candidate":
                return evidence
            if evidence["status"] == "submitting":
                raise ValueTransferError(
                    "transfer submission was interrupted before its hash was journaled; reconcile before retry"
                )
        else:
            before = {"sender": ffi.snapshot(sender), "recipient": ffi.snapshot(recipient)}
            evidence = {
                **inspection,
                "status": "submitting",
                "state": {"before": before},
                "prepared_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
            }
            lez_wallet.atomic_json(args.evidence, evidence)
            with lez_bond.captured_native_output() as captured:
                transaction = ffi.submit_transfer(sender, recipient, words, guest)
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
        inclusion = lez_bond.wait_for_finalized(evidence["transaction"], args.timeout)
        after = {"sender": ffi.snapshot(sender), "recipient": ffi.snapshot(recipient)}
        before = evidence["state"]["before"]
        if int(after["sender"]["balance"]) != int(before["sender"]["balance"]) - args.amount:
            raise ValueTransferError("sender balance does not reflect the exact transfer")
        if int(after["recipient"]["balance"]) != int(before["recipient"]["balance"]) + args.amount:
            raise ValueTransferError("recipient balance does not reflect the exact transfer")
        evidence.update(inclusion)
        evidence["status"] = "official-wallet-sequencer-finalized-candidate"
        evidence["state"]["after"] = after
        evidence["observed_at_utc"] = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
        evidence["verification_boundary"] = (
            "Signed application authorization, official-wallet real proof, sequencer finality, "
            "and exact public balance movement; explorer promotion is separate."
        )
        lez_wallet.atomic_json(args.evidence, evidence)
        return evidence
    finally:
        ffi.close()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Official-wallet LEZ use-case transfer adapter")
    commands = result.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("generate-signing-key")
    generate.add_argument("--private-key", type=Path, required=True)
    create = commands.add_parser("create-authorization")
    create.add_argument("--operation", choices=tuple(OPERATIONS), required=True)
    create.add_argument("--profile", required=True)
    create.add_argument("--sender", required=True)
    create.add_argument("--recipient", required=True)
    create.add_argument("--amount", type=int, required=True)
    create.add_argument("--nonce", required=True)
    create.add_argument("--created-at-ms", type=int, required=True)
    create.add_argument("--expires-at-ms", type=int, required=True)
    create.add_argument("--claims", type=Path, required=True)
    create.add_argument("--evidence", type=Path, required=True)
    sign = commands.add_parser("sign-authorization")
    sign.add_argument("--authorization", type=Path, required=True)
    sign.add_argument("--private-key", type=Path, required=True)
    sign.add_argument("--role", choices=("policy-owner", "owner", "requester", "provider"), required=True)
    sign.add_argument("--evidence", type=Path, required=True)
    execute_command = commands.add_parser("execute")
    execute_command.add_argument("--network-profile", type=Path, default=lez_wallet.DEFAULT_PROFILE)
    execute_command.add_argument("--wallet-source", type=Path, required=True)
    execute_command.add_argument("--wallet-home", type=Path, required=True)
    execute_command.add_argument("--operation", choices=tuple(OPERATIONS), required=True)
    execute_command.add_argument("--profile", required=True)
    execute_command.add_argument("--sender", required=True)
    execute_command.add_argument("--recipient", required=True)
    execute_command.add_argument("--amount", type=int, required=True)
    execute_command.add_argument("--authorization", type=Path, required=True)
    execute_command.add_argument("--trusted-signers", type=Path, required=True)
    execute_command.add_argument("--submit", action="store_true")
    execute_command.add_argument("--timeout", type=float, default=1800.0)
    execute_command.add_argument("--evidence", type=Path, required=True)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "generate-signing-key":
            response = generate_signing_key(args.private_key)
        elif args.command == "create-authorization":
            response = create_authorization(args)
        elif args.command == "sign-authorization":
            response = sign_authorization(args)
        else:
            if args.timeout <= 0:
                raise ValueTransferError("timeout must be positive")
            response = execute(args)
        print(json.dumps(response, sort_keys=True))
        return 0
    except (
        ValueTransferError,
        lez_bond.BondAdapterError,
        lez_wallet.WalletAdapterError,
        OSError,
        ValueError,
    ) as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
