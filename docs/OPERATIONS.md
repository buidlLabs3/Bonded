# Operations

## Build And Test

```bash
nix build .#lib -L
nix build .#lgx -L
nix build .#basecamp-lgx -L
nix develop --command bash scripts/run-unit-tests.sh
nix develop --command bash scripts/run-service-tests.sh
nix develop --command bash scripts/run-second-half-tests.sh
nix develop --command bash scripts/run-skill-runtime-tests.sh
scripts/run-python-tests.sh
nix run .#basecamp-preview-smoke
```

Run the fixture-backed Basecamp UI with `nix run .#basecamp-preview`. It is an
interaction test, not a wallet or testnet client.

Run the live owner app with `nix run .#basecamp`. Copy the controller public key
shown in Settings and use it as `OWNER_PUBLIC_KEY` when deploying the agents
below. After their cards are discovered, select the Vault agent in Tools to
upload/download encrypted content, or the Settlement agent to inspect a LEZ
account. These commands and their replies use the encrypted owner channel.

## Headless Deployment

Build the pinned official stack. The command runs sequentially and writes only
Nix result links beneath the chosen output directory.

```bash
scripts/build-live-stack.sh build/live-stack
```

Deploy three isolated profiles. Each profile needs a distinct private-owned LEZ
account in a distinct wallet directory outside the repository; wallet files
cannot be shared by concurrently running Core processes.

```bash
scripts/manage-live-agents.sh deploy \
  --artifacts build/live-stack \
  --data-root /absolute/private/path/to/agents \
  --owner-public-key "$OWNER_PUBLIC_KEY" \
  --wallet-home-inbox "$INBOX_WALLET_HOME" \
  --lez-account-id-inbox "$INBOX_ACCOUNT_ID_HEX" \
  --wallet-home-vault "$VAULT_WALLET_HOME" \
  --lez-account-id-vault "$VAULT_ACCOUNT_ID_HEX" \
  --wallet-home-settlement "$SETTLEMENT_WALLET_HOME" \
  --lez-account-id-settlement "$SETTLEMENT_ACCOUNT_ID_HEX"
```

Each profile receives its own runtime identity, SQLite database, Storage data,
Core persistence, and log. A paid Agent Card signs the account ID plus its public
nullifier/viewing keys so another agent can use LEZ's cross-wallet private
transfer; recovery material and private keys never enter the card. Inspect or
stop all three without deleting state:

```bash
scripts/manage-live-agents.sh health --data-root /absolute/private/path/to/agents
scripts/manage-live-agents.sh status --data-root /absolute/private/path/to/agents
scripts/manage-live-agents.sh stop --data-root /absolute/private/path/to/agents
scripts/manage-live-agents.sh start --data-root /absolute/private/path/to/agents
```

For a single profile, `bin/bonded-inbox --help` exposes `plan`, `deploy`,
`start`, `stop`, `status`, `health`, `logs`, `invoke`, and guarded test-only
`teardown` commands.

## Pre-Video Acceptance

Use a fresh private agent data root and keep all three agents running while
Basecamp is open. The agent selector must show three distinct signed identities.
Run these flows without fixture mode:

1. **Personal file vault:** select the Vault agent in Tools, upload private text,
   record the returned address, then download it. Stop and restart only the Vault
   profile and download the same address again.
2. **Paid skill marketplace:** publish cards, discover the provider from a second
   agent, create a priced task, complete it, and retain the signed receipt plus
   finalized LEZ transaction. Repeat a request ID to confirm replay rejection;
   cancel a separate unfinished task to confirm no payment is duplicated.
3. **On-chain event alert:** select the Settlement agent, observe a known account,
   then use the Inbox agent to send the encrypted result to the owner. Stop the
   Inbox agent before one attempt, restart it, and confirm queued delivery occurs
   once.

After every flow, capture the public result only. Do not capture plaintext,
wallet paths, owner keys, recovery material, or the contents of agent databases.
The release is not video-ready unless all commands below succeed and every open
criterion in `release/submission-readiness.json` has been updated with real
evidence:

```bash
scripts/manage-live-agents.sh health --data-root "$AGENT_DATA_ROOT"
scripts/verify-lez-evidence.sh
python3 tools/traceability_gate.py
python3 tools/traceability_gate.py --criterion LP8-THREE-AGENTS
python3 tools/traceability_gate.py --criterion LP8-OWNER-CHANNEL
python3 tools/traceability_gate.py --criterion LP8-DEFAULT-SKILLS
python3 tools/traceability_gate.py --criterion LP8-A2A-PAID-TASK
python3 tools/traceability_gate.py --criterion LP8-USE-CASES
python3 tools/traceability_gate.py --criterion LP8-COMPUTE-UNITS
```

## Official LEZ Testnet

Release transactions use `https://testnet.lez.logos.co` and the official LEZ
v0.2.4 wallet at commit
`47eba256479f6f785acbd138834340703cd03401`. Wallet state must be outside the
repository and private.

```bash
export BONDED_LEZ_SOURCE=/absolute/path/to/logos-execution-zone
export LEE_WALLET_HOME_DIR=/absolute/private/path/to/wallet
python3 tools/lez_testnet.py preflight
python3 tools/lez_wallet.py check-wallet \
  --wallet-source "$BONDED_LEZ_SOURCE" \
  --wallet-home "$LEE_WALLET_HOME_DIR"
```

Create and initialize the three shielded agent wallets one at a time. Wallet
creation never overwrites a directory and suppresses native recovery/key output;
store each recovery file separately before continuing. The private claim both
initializes and funds the named agent account with a finalized LEZ transaction.

```bash
export AGENT_WALLET_ROOT=/absolute/private/path/to/agent-wallets
for profile in inbox vault settlement; do
  BONDED_LEZ_BOOTSTRAP=YES python3 tools/lez_wallet_bootstrap.py \
    --wallet-source "$BONDED_LEZ_SOURCE" \
    --wallet-home "$AGENT_WALLET_ROOT/$profile" \
    --agent-profile "$profile" --create
  RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
  python3 tools/lez_wallet_provision.py \
    --wallet-source "$BONDED_LEZ_SOURCE" \
    --wallet-home "$AGENT_WALLET_ROOT/$profile" \
    --agent-profile "$profile" --submit --timeout 43200 \
    --prover ipc --rayon-threads 2 \
    --evidence "$AGENT_WALLET_ROOT/$profile/provisioning.json" \
    initialize-agent
done
```

Use each manifest's public `agent.id_hex` value for the matching
`--lez-account-id-*` deployment argument. Do not expose recovery phrases,
wallet storage, or private keys in logs or repository evidence.

The corrected settlement deployment is transaction
`fc88b2bad2b51026fb97c6cc8b4943ead59f8a3cc0e515f9f058f9e49fb11ea9`
in finalized block `6001`, program
`50ce86eebf3a01a5febe8cc735895adf361c2fa43a14947277e3d1050fbdcb8b`.
The sender, owner, and sink registration transactions are respectively
`2e64e58926ec14b4b59c477c7bd0f63a824e4f7122f8410ee9fd0a1ea14ba259`,
`9f8e1461b7f3a3dc805a392811d8849e67d65d099bde25c88639ff85f22c7c16`,
and `2f1df54d2658dc6ee45ae3eaac43902fcf548462cba7ff6599ca0670abc759ac`.
Sender funding is
`05fb87d3eda87fad090eafd1b5cff7b001faf8df594eb81ba74ee00e5e4f9b15`
in block `5661` for 150 units.

Run lifecycle calls sequentially with one local prover:

```bash
RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
python3 tools/lez_bond_matrix.py --submit --wait-for-expiry \
  --wallet-source "$BONDED_LEZ_SOURCE" --wallet-home "$LEE_WALLET_HOME_DIR" \
  --sender "$SENDER" --owner "$OWNER" --sink "$SINK" \
  --release-commit "$(git rev-parse HEAD)" --amount 10 \
  --expiry-delay-ms 28800000 --timeout 43200 \
  --prover ipc --rayon-threads 2 \
  --journal evidence/testnet/candidates/bond-matrix-v2.json
```

Run the three signed value transfers only after the lifecycle matrix stops using
the prover:

```bash
RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
python3 tools/lez_value_matrix.py --submit \
  --wallet-source "$BONDED_LEZ_SOURCE" --wallet-home "$LEE_WALLET_HOME_DIR" \
  --sender "$SENDER" --owner "$OWNER" --sink "$SINK" \
  --release-commit "$(git rev-parse HEAD)" \
  --timeout 43200 --prover ipc --rayon-threads 2 \
  --journal evidence/testnet/candidates/value-matrix-v2.json
```

An interrupted `submitting` candidate must be reconciled; never retry it by
guessing. After finality, use `tools/lez_bond_evidence.py` or
`tools/lez_value_evidence.py` for two observations separated by at least three
finalized blocks. Capture the exact official explorer transaction and block
pages with `tools/lez_explorer_capture.mjs`.

Run the complete read-only live gate with:

```bash
scripts/verify-lez-evidence.sh
```

Every required row needs two distinct finalized observations and transaction
and block screenshots. Candidate files, local proofs, and sequencer-only hashes
do not qualify a public transaction. The standalone 5/5 real-proof artifact is
`evidence/standalone/official-wallet-real-proof-v2.json`; its declared scope is
local standalone, not public testnet.

The live module waits for `lez_core.poll_transaction_status` before returning a
transaction hash. That confirms sequencer inclusion only; use the evidence gate
above to confirm finalized explorer visibility.

Official compute-unit cost must come from the official wallet, sequencer, or
explorer receipt for the exact finalized transaction. The current 16-operation
evidence set does not expose a numeric CU field, so local timings in
`tools/benchmark_local.py` must not be reported as CU cost.

## Recovery

Stop only the affected profile, preserve its data directory, run `health`, and
take a backup before repair. Verify disk space, module checksum, network, and
dependency reachability. Restart with the same profile and data directory, then
reconcile outbox records, bonds, tasks, and receipts. Never create a replacement
bond or payment ID for an uncertain operation.

Restore corrupted configuration or SQLite state from a known backup. Do not
silently recreate state while bonds are outstanding. Owner unavailability leaves
approvals pending until timeout; it never authorizes a spend.

## Release Status

| ID | Requirement | Scope | Evidence | Status |
|---|---|---:|---|---|
| UI-01 | Runnable Basecamp owner interface | local | QML smoke and asset tests | verified-local |
| CI-02 | Real proof with development mode disabled | standalone | five-case v2 artifact | verified-local |
| LEZ-BASE | Program and wallet baseline evidence | testnet | deployment, registrations, funding | verified-testnet |
| LEZ-LIFECYCLE | Acceptance, rejection, expiry, delivery failure | testnet | eight required transactions | verified-testnet |
| LEZ-VALUE | Spending control and paid task settlement | testnet | three required transactions | verified-testnet |

These testnet rows prove only the 16 public operations indexed by
`evidence/testnet/required-evidence.json`. They do not claim that three agents,
the Basecamp backend, or the live A2A flow are deployed. The criterion-by-criterion
status and explicit gaps are recorded in `release/submission-readiness.json`.

Known release limitations are the absence of a dynamic untrusted-skill loader,
the fixture-only scope of `basecamp-preview`, and the need for an external
self-hosted runner for the bounded real-proof CI job. Public testnet finality,
live Logos Core behavior, and CI status cannot be inferred from local adapter
tests.
