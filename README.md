# Bonded Inbox

Private first-contact messaging where an unknown sender locks a refundable LEZ
bond before the message reaches the recipient. Accepted, expired, and
delivery-failed messages refund the sender. Explicit spam rejection sends the
bond to the policy's fixed community sink, never to the inbox owner.

This repository targets the Logos Lambda Prize LP-0008. It is experimental
testnet software and must not be used with production funds or identities.

## Current State

The repository contains a buildable Logos Core module and `.lgx` package,
signed/versioned inbox policies, encrypted storage, persisted message and bond
state machines, spending approvals, safe local triage, signed A2A cards and paid
task coordination, and exactly 21 profile-scoped default skills. It also ships
an idempotent headless operations CLI, Basecamp QML assets, recovery primitives,
a pinned RISC Zero LEZ settlement guest, and local conformance tests.

The official Logos Messaging and Storage adapters, shielded LEZ wallet/runtime
binding, live Basecamp replica, three agent-profile deployments, contract
invocation proofs, CU measurements, and narrated video remain release gates.
Local adapters are never represented as testnet evidence. See
`docs/known-limitations.md` and `docs/requirements/traceability.md` for the exact
qualification state.

## Build

Prerequisites: Nix with flakes enabled and Git. The first build resolves the
pinned Logos module toolchain and may take several minutes.

```bash
nix build .#lib -L
nix build .#lgx -L
nix build .#basecamp-lgx -L
```

The plugin build produces `lib/bonded_inbox_plugin.so`. The package build
produces `logos-bonded_inbox-module-lib.lgx` through the `result` link.
The Basecamp build separately produces `logos-bonded_inbox_ui-module.lgx`
through the upstream builder's supported QML-module packaging path.

## Test

```bash
nix develop --command bash scripts/run-unit-tests.sh
nix develop --command bash scripts/run-service-tests.sh
nix develop --command bash scripts/run-second-half-tests.sh
nix develop --command bash scripts/run-skill-runtime-tests.sh
scripts/run-python-tests.sh
cargo test --locked --manifest-path programs/bonded-inbox/Cargo.toml
cargo check --locked --manifest-path programs/bonded-inbox/lez-guest/Cargo.toml
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

## Headless Operations

Plan and deploy with explicit local artifacts:

```bash
bin/bonded-inbox --data-dir ./agent-data plan \
  --profile inbox --network logos-local \
  --owner-public-key OWNER_PUBLIC_KEY \
  --module ./result/logos-bonded_inbox-module-lib.lgx \
  --core-binary /path/to/logos-core
```

Replace `plan` with `deploy` after reviewing preflight output. The CLI also
supports `status`, `health`, `logs`, `policy`, `fund`, `approve`, `deny`,
`backup`, `restore`, `upgrade`, `rollback`, and guarded test teardown. See
`docs/deployment/headless.md`.

Basecamp assets live under `basecamp/`; the owner workflow is documented in
`docs/owner-guide.md`. The 21-operation reference is
`docs/reference/default-skills.md`, and the local end-to-end test command is
`nix develop --command bash scripts/demo-local.sh`.

## LEZ Testnet Program

The deployment path targets the official LEZ v0.2.4 testnet ABI and fails closed
if the endpoint's built-in program IDs drift from that release.

```bash
python3 tools/lez_testnet.py preflight
RISC0_DEV_MODE=0 scripts/build-lez-program.sh
RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
BONDED_LEZ_SOURCE=/absolute/path/to/logos-execution-zone \
BONDED_LEZ_WALLET_HOME=/absolute/private/path/to/wallet \
scripts/deploy-lez-testnet.sh
```

The final command uses the pinned official wallet to publish the settlement ELF
and writes sanitized candidate evidence under `evidence/testnet/candidates/`.
The official CLI currently initializes wallet storage before executing even an
unsigned deployment, so use the external wallet directory and secret-safe setup
in `docs/deployment/lez-wallet.md`. The build requires Docker, `cargo-risczero`,
`r0vm`, Rust, and network access on its first run.

Publishing the guest is not equivalent to deploying the Inbox, Vault, and
Settlement agent profiles. Those profiles require official Core integrations,
three independent shielded identities, an owner public key, and successful
testnet contract invocations. See `docs/deployment/lez-testnet.md`.

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
