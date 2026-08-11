# A2A Messaging Binding

Bonded Inbox pins the protocol identifier `lf.a2a.v1`, matching the package name
used by the pinned Logos A2A repository revision recorded in
`docs/architecture/upstream-compatibility.md`.

## Transport

Agent Cards and task updates are carried inside signed
`bonded-inbox/envelope/v1` messages. The envelope supplies network binding,
sender and recipient identities, topic, unique ID, nonce, expiry, Ed25519 public
key, and signature. Encryption and delivery are delegated to Logos Messaging in
a production adapter; the checked-in memory adapter is local test evidence only.

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
