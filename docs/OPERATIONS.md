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

## Headless Deployment

Use separate data directories and identities for `inbox`, `vault`, and
`settlement`.

```bash
bin/bonded-inbox --data-dir /var/lib/bonded-inbox plan \
  --profile inbox --network lez-testnet \
  --owner-public-key OWNER_PUBLIC_KEY \
  --module ./result/logos-bonded_inbox-module-lib.lgx \
  --core-binary /opt/logos/bin/logos-core

bin/bonded-inbox --data-dir /var/lib/bonded-inbox deploy \
  --profile inbox --network lez-testnet \
  --owner-public-key OWNER_PUBLIC_KEY \
  --module ./result/logos-bonded_inbox-module-lib.lgx \
  --core-binary /opt/logos/bin/logos-core
```

The CLI also provides `status`, `health`, `logs`, `policy`, `fund`, `approve`,
`deny`, `backup`, `restore`, `upgrade`, and `rollback`. Review `--help` before
mutating a deployment.

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
| LEZ-BASE | Program and wallet baseline evidence | testnet | deployment, registrations, funding | implemented |
| LEZ-LIFECYCLE | Acceptance, rejection, expiry, delivery failure | testnet | eight required transactions | planned |
| LEZ-VALUE | Spending control and paid task settlement | testnet | three required transactions | planned |
