# Bonded Inbox

Private first-contact messaging where an unknown sender locks a refundable LEZ
bond before the message reaches the recipient. Accepted, expired, and
delivery-failed messages refund the sender. Explicit spam rejection sends the
bond to the policy's fixed community sink, never to the inbox owner.

This repository targets the Logos Lambda Prize LP-0008. It is experimental
testnet software and must not be used with production funds or identities.

## Current State

The repository contains a buildable Logos Core module foundation, generated
universal interface, `.lgx` packaging, signed/versioned policies, encrypted local
storage primitives, transactional workflow persistence, message and bond state
machines, private settlement receipts, profile-scoped skills, spending approval
controls, injectable stack adapters, and an allocation-free Rust bond-program
core.

The real Logos Storage, Delivery, shielded LEZ wallet, LEZ guest wrapper,
Basecamp UI, testnet deployments, and `RISC0_DEV_MODE=0` evaluator environment
remain release gates. See `docs/requirements/first-half-checkpoint.md`; local
test doubles are never represented as testnet evidence.

## Build

Prerequisites: Nix with flakes enabled and Git. The first build resolves the
pinned Logos module toolchain and may take several minutes.

```bash
nix build .#lib -L
nix build .#lgx -L
```

The plugin build produces `lib/bonded_inbox_plugin.so`. The package build
produces `logos-bonded_inbox-module-lib.lgx` through the `result` link.

## Test

```bash
nix develop --command bash scripts/run-unit-tests.sh
nix develop --command bash scripts/run-service-tests.sh
cargo test --manifest-path programs/bonded-inbox/Cargo.toml
nix build .#generate -L
```

The standalone runners intentionally use `pkg-config` for OpenSSL and SQLite.
This avoids an upstream development-shell quoting bug when a local workspace
path contains spaces; the hermetic Nix module build is unaffected.

## Module API

The generated module interface currently exposes:

- `initialize(configuration_json)`
- `getStatus()`
- `publishPolicy(policy_json)`
- `submitMessage(submission_json)`
- `decideMessage(decision_json)`
- `invokeSkill(request_json)`

It emits `stateChanged(event_json)` and
`ownerActionRequired(proposal_json)`. Public methods use canonical JSON strings
to keep unstable upstream C++ types behind adapters.

## Profiles

- `profiles/inbox.json`: Messaging deployment and owner review.
- `profiles/vault.json`: Storage deployment and attachment grants.
- `profiles/settlement.json`: Blockchain deployment, bond settlement, and
  receipts.

All profiles use the same runtime with independent identity, configuration,
data directory, and explicit capability allowlist.

## Security

Message content never belongs on-chain. Policy and message commitments are
SHA-256; signed documents use Ed25519; local authenticated encryption uses
AES-256-GCM through OpenSSL EVP. Production keys must come from the Logos/LEZ
identity provider. Model output cannot reject a bonded message without an
explicit owner action or deterministic policy violation.

See `SECURITY.md`, the ADRs under `docs/architecture`, and the live requirement
traceability matrix under `docs/requirements`.

## License

MIT
