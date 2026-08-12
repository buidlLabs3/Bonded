# A2A Messaging Binding

Bonded Inbox pins the protocol identifier `lf.a2a.v1`, matching the package name
used by the pinned Logos A2A repository revision recorded in
`docs/architecture/upstream-compatibility.md`.

## Transport

Agent Cards and task updates are carried inside signed
`bonded-inbox/envelope/v2` messages. The envelope supplies network binding,
sender and recipient identities, topic, unique ID, replay nonce, expiry,
ephemeral X25519 public key, AES-256-GCM ciphertext/nonce/tag, Ed25519 public
key, and signature. Each message derives a fresh context-bound encryption key
with HKDF-SHA-256;
the signature covers the sealed envelope. Receivers require the exact recipient
identity and a pinned sender signing key before decryption. Logos Messaging
provides delivery in a production adapter; the checked-in shared-memory bus is
local two-instance protocol evidence only.

Version 1 is rejected because it carried a signed plaintext payload. Peers
discover the recipient X25519 public key in the signed Agent Card's
`messaging_encryption_public_key` capability. Private X25519 and Ed25519 keys
remain local.

## Agent Cards

The signed card covers protocol, network, agent ID, public key, skill names,
capabilities, discovery topic, optional task price, and expiry. Discovery rejects
wrong-network, expired, unsigned, or tampered cards and deduplicates by agent ID.

## Tasks And Payment

States are `working`, `input-required`, `completed`, `failed`, and `canceled`.
Creation locks the exact signed-card price through the program adapter.
Completion releases once. Failure and cancellation request a refund once.
Conflicting terminal transitions fail. Repeated create, complete, and cancel
operations return the recorded state.

The `bonded-a2a-escrow` program identifier in local tests is an adapter boundary,
not a claimed testnet deployment. Program IDs and proof references must be
replaced by verified evidence before release qualification.
