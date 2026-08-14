# Bonded Inbox

Bonded Inbox is a Logos Core module for private first-contact messaging. An
unknown sender locks a refundable LEZ bond before delivery. Acceptance, expiry,
and delivery failure refund the sender; explicit rejection pays the configured
community sink, never the inbox owner.

This is testnet software. Do not use production funds, identities, or messages.

## Run Locally

Prerequisites are Git and Nix with flakes enabled.

```bash
nix build .#lib -L
nix build .#lgx -L
nix build .#basecamp-lgx -L
scripts/run-python-tests.sh
```

Run the Basecamp owner UI with fixture data:

```bash
nix run .#basecamp-preview
```

Run its headless instantiation check:

```bash
nix run .#basecamp-preview-smoke
```

The preview supports inbox decisions, task review, spending approvals, settings,
refresh, and offline/loading/empty states. It does not submit testnet actions.

## Module

The generated module interface exposes:

- `initialize(configuration_json)`
- `getStatus()`
- `publishPolicy(policy_json)`
- `submitMessage(submission_json)`
- `decideMessage(decision_json)`
- `invokeSkill(request_json)`

It emits `stateChanged(event_json)` and
`ownerActionRequired(proposal_json)`. The Inbox, Vault, and Settlement profiles
use separate identities, data directories, and capability allowlists.

The runtime includes encrypted storage, persisted bond/message state, spending
approval, A2A task/payment coordination, recovery handling, and 21
profile-scoped skills across messaging, storage, blockchain, A2A, and metadata.

## Testnet

The repository pins official LEZ v0.2.4 commit
`47eba256479f6f785acbd138834340703cd03401` and rejects endpoint or built-in
program drift. Release transactions use the pinned official wallet with
`RISC0_DEV_MODE=0`; locally generated hashes are not public testnet evidence.

The corrected settlement program is
`50ce86eebf3a01a5febe8cc735895adf361c2fa43a14947277e3d1050fbdcb8b`.
Its official deployment transaction is
`fc88b2bad2b51026fb97c6cc8b4943ead59f8a3cc0e515f9f058f9e49fb11ea9`
in finalized block `6001`.

See [docs/OPERATIONS.md](docs/OPERATIONS.md) for deployment, wallet, evidence,
and recovery commands. See [SECURITY.md](SECURITY.md) before handling wallet or
message data and [CONTRIBUTING.md](CONTRIBUTING.md) before changing code.

## License

MIT
