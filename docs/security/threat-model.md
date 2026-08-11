# Threat Model

Assets are shielded identity material, LEZ funds, signed policy authority,
message and attachment plaintext, storage keys, owner decisions, A2A payments,
and recovery data. Adversaries include malicious senders, owners, agents,
messaging peers, storage operators, chain observers, compromised dependencies,
and local users.

Trust boundaries exist at every JSON/network/file input, adapter call, owner
signature, policy transition, payment transition, QML replica update, and CLI
archive path. Controls include Ed25519 domain-separated signatures, AES-GCM with
associated data, immutable policy hashes, fixed sinks, monotonic revisions,
idempotency keys, profile allowlists, bounded queues, safe archive extraction,
mode-`0700` deployment directories, secret-safe output, and fail-closed external
evidence gates.

Model content is never parsed as a command. A score cannot move funds. Only a
known deterministic rule or verified owner intent can authorize rejection.
External adapters remain untrusted until their response is validated against
network, identity, amount, and state expectations.
