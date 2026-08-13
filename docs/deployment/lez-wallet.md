# Official LEZ Wallet Runbook

Bonded Inbox pins the official LEZ wallet from
`logos-blockchain/logos-execution-zone` release v0.2.4 at commit
`47eba256479f6f785acbd138834340703cd03401`. The validated network profile is
`config/lez-testnet-network.json`. Release transactions must use this wallet or
its public `WalletCore` transaction-building APIs; `tools/lez_testnet.py` is a
conformance oracle only and cannot create release evidence.

## Build And Provenance

Use a clean checkout outside this repository:

```bash
export BONDED_LEZ_SOURCE=/absolute/path/to/logos-execution-zone
scripts/build-official-lez-wallet.sh
python3 tools/lez_wallet.py check-source --wallet-source "$BONDED_LEZ_SOURCE"
```

The check rejects the wrong commit, a non-official origin, tracked source
changes, a missing release binary, an unexecutable binary, or missing official
FFI artifacts. Its sanitized output includes the source commit plus SHA-256/size
for the CLI, generated FFI header, and `libwallet_ffi.so`. The generic Bonded
lifecycle adapter uses this exact official library; it does not load a system or
unpinned wallet library.

Supported upstream surfaces at the pinned commit are:

| Operation | Official command or API |
|---|---|
| Program deployment | `wallet deploy-program <binary>` / `WalletCore::send_program_deployment_transaction` |
| Public account creation | `wallet account new public` |
| Private account creation | `wallet account new private` |
| Public account initialization | `wallet auth-transfer init --account-id Public/...` |
| Testnet funding | `wallet pinata claim --to Public/...` or `Private/...` |
| Public invocation | `WalletCore::send_pub_tx` |
| Privacy-preserving invocation | `WalletCore::send_privacy_preserving_tx` |

The official CLI does not expose a generic arbitrary-program call command. The
Bonded adapter must use the public `WalletCore` APIs for those calls and must
fail closed if the required account or instruction shape is unsupported.

`tools/lez_bond.py` is the typed wrapper for those generic calls. It verifies
the exact FFI artifact and canonical guest image ID, derives the state and
escrow PDAs through the official wallet, and emits the instruction words whose
layout is checked by a Rust guest serialization oracle. Omit `--submit` to
inspect the complete call shape without proving or sending it:

```bash
python3 tools/lez_bond.py \
  --wallet-source "$BONDED_LEZ_SOURCE" \
  --wallet-home "$LEE_WALLET_HOME_DIR" \
  --sender "$SENDER" --owner "$OWNER" --sink "$SINK" \
  --bond-id "$BOND_ID" \
  initialize \
  --message-commitment "$MESSAGE_COMMITMENT" \
  --policy-commitment "$POLICY_COMMITMENT" \
  --amount 1000 --deadline-ms "$DEADLINE_MS"
```

Submission additionally requires `--submit`, `BONDED_LEZ_SUBMIT=YES`, and
`RISC0_DEV_MODE=0`. Native wallet output is captured in a private temporary
file, scanned for recovery/key material, reduced to its SHA-256 and byte count,
and deleted. The adapter journals `submitting` before entering the wallet FFI and
persists the exact returned hash as `submitted` before finality polling. Repeating
the identical command after a polling timeout observes that hash without sending
another transaction. An interrupted `submitting` state blocks automatic retry
until it is manually reconciled. Output remains a sequencer-finalized candidate
until the explorer gate independently promotes it.

After a lifecycle candidate reaches sequencer finality and account-state
reconciliation, promote the exact journaled hash through the public explorer:

```bash
python3 tools/lez_bond_evidence.py \
  --candidate evidence/testnet/candidates/acceptance-initialize.json \
  --deployment evidence/testnet/deployment/settlement-program-v2-primary.json \
  --operation acceptance-initialize \
  --verifier-commit "$(git rev-parse HEAD)" \
  --observer primary \
  --evidence evidence/testnet/lifecycle/acceptance-initialize.json
```

The corrected v2 deployment is also the helper's fail-closed default. The
explicit argument above makes that release binding visible in audit logs. The
helper requires the canonical deployment's program and binary, checks the
operation/outcome account shape, compares the finalized hash and block back to
the lifecycle journal, and creates the observation immutably. Use a distinct
observer and output path for the later independent observation.

For the complete public matrix, `tools/lez_bond_matrix.py` creates deterministic
privacy-safe commitments for four fresh bonds and runs the eight successful
calls sequentially. It initializes the expiry case first, then acceptance,
rejection, and delivery failure, using their proof time before the final expiry
refund. Every completed step must have its own finalized candidate before the
runner advances. The matrix journal makes a timeout resumable and refuses an
ambiguous retry:

```bash
RAYON_NUM_THREADS=1 RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
python3 tools/lez_bond_matrix.py --submit --wait-for-expiry \
  --wallet-source "$BONDED_LEZ_SOURCE" --wallet-home "$LEE_WALLET_HOME_DIR" \
  --sender "$SENDER" --owner "$OWNER" --sink "$SINK" \
  --release-commit "$(git rev-parse HEAD)" --amount 10 \
  --expiry-delay-ms 28800000 --timeout 21600 \
  --journal evidence/testnet/candidates/bond-matrix-v2.json
```

Run it only after the exact corrected deployment is finalized and explorer
reconciled. Use one proof process at a time. The runner does not turn rejected
early-expiry, unauthorized, replay, or conflict attempts into successful
evidence; those negative cases require separate no-state-change records.

## Spending And Paid-Task Transfers

`tools/lez_value_transfer.py` binds the three value-moving application claims to
real official-wallet privacy-preserving transfers. It loads the authenticated
transfer ELF from the pinned FFI, derives and verifies its canonical image ID,
serializes the exact `Transfer { amount }` instruction, and reconciles sender and
recipient balances before and after finality. The supported operations and
required application attestations are:

| Operation | Required Ed25519 roles | Required claim |
|---|---|---|
| `below-limit-transfer` | `policy-owner` | amount is within both signed policy limits |
| `owner-approved-transfer` | `owner` | proposal ID and amount above the per-transaction limit |
| `paid-task-settlement` | `requester`, `provider` | completed task ID, parties, and skill |

Create a claims JSON object, then create and sign the authorization. Signing
keys are raw 32-byte Ed25519 private keys held in external mode-`0600` files;
only public keys and signatures enter the authorization artifact. Use a validity
window long enough for real proof generation:

```bash
install -d -m 0700 /external/private/bonded-value-signers
python3 tools/lez_value_transfer.py generate-signing-key \
  --private-key /external/private/bonded-value-signers/policy-owner.key
python3 tools/lez_value_transfer.py create-authorization \
  --operation below-limit-transfer --profile settlement \
  --sender "$SENDER" --recipient "$RECIPIENT" --amount 2 \
  --nonce "$NONCE" --created-at-ms "$CREATED_AT_MS" \
  --expires-at-ms "$EXPIRES_AT_MS" --claims /path/to/claims.json \
  --evidence /path/to/unsigned-authorization.json
python3 tools/lez_value_transfer.py sign-authorization \
  --authorization /path/to/unsigned-authorization.json \
  --private-key /external/private/policy-owner.key --role policy-owner \
  --evidence evidence/testnet/authorizations/below-limit-transfer.json
```

Key generation is exclusive: the command refuses an existing destination, a
symlink, or a group/world-accessible parent directory and prints only the public
key. Store that public key in the operation's trusted signer map before signing.

The paid-task operation repeats the signing command sequentially, passing the
first signed artifact as the second command's input and using separate
`requester` and `provider` keys. Execute only after reviewing the signed payload:

```bash
RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
python3 tools/lez_value_transfer.py execute --submit \
  --wallet-source "$BONDED_LEZ_SOURCE" --wallet-home "$LEE_WALLET_HOME_DIR" \
  --operation below-limit-transfer --profile settlement \
  --sender "$SENDER" --recipient "$RECIPIENT" --amount 2 \
  --authorization evidence/testnet/authorizations/below-limit-transfer.json \
  --trusted-signers /external/config/trusted-value-signers.json \
  --timeout 21600 \
  --evidence evidence/testnet/candidates/below-limit-transfer.json
```

The trusted signer map is an external JSON object from required role to pinned
Ed25519 public key. It must contain exactly the roles required by the operation.
The Ed25519 signatures prove the application decision; the sender account
signature inside the official wallet transaction separately authorizes the LEZ
value movement. After sequencer finality, promote the candidate without opening
the wallet:

```bash
python3 tools/lez_value_evidence.py \
  --candidate evidence/testnet/candidates/below-limit-transfer.json \
  --authorization evidence/testnet/authorizations/below-limit-transfer.json \
  --trusted-signers /external/config/trusted-value-signers.json \
  --operation below-limit-transfer \
  --verifier-commit "$(git rev-parse HEAD)" --observer primary \
  --evidence evidence/testnet/spending/below-limit-transfer.json
```

Use a later observation with a distinct observer, then capture the exact
transaction and block explorer pages. Application attestations do not reveal or
replace the LEZ wallet key, and no signing private key belongs in this repository.

## Secret-Safe Wallet Setup

Create a dedicated testnet-only wallet directory outside the repository. Set
its directory mode to `0700`. The pinned v0.2.4 wallet does not encrypt its
storage at rest, so directory and file permissions are a hard security boundary.
The bootstrap wrapper calls the official FFI, writes the recovery phrase without
printing it, and creates distinct sender, owner, and sink public identities.
Use a new absolute path and immediately transfer the recovery phrase from the
generated `0600` file into an approved secret manager.
Bootstrap writes the pinned upstream polling defaults and uses five initial
leader-calibration samples, matching the upstream integration-fixture bound;
it does not change the endpoint, finality, or transaction-polling contract.

```bash
export LEE_WALLET_HOME_DIR=/absolute/private/path/bonded-lez-testnet-wallet
BONDED_LEZ_BOOTSTRAP=YES python3 tools/lez_wallet_bootstrap.py \
  --create \
  --wallet-source "$BONDED_LEZ_SOURCE" \
  --wallet-home "$LEE_WALLET_HOME_DIR"
python3 tools/lez_wallet.py check-wallet \
  --wallet-source "$BONDED_LEZ_SOURCE" \
  --wallet-home "$LEE_WALLET_HOME_DIR"
```

The adapter rejects a repository-local or symlinked wallet directory,
group/world-readable storage, endpoint credentials, multiple sequencers, and
any sequencer other than the pinned official testnet endpoint. Never commit
`storage.json`, mnemonic material, private keys, nullifier secret keys, or raw
wallet logs.

For signed testnet calls, create dedicated public and private accounts using the
official CLI. Initialize the funding account, then use the testnet Piñata
program. Faucet availability, cooldown, and balance are external prerequisites;
check them immediately before a lifecycle run. Only public account IDs intended
for evidence may be recorded. Private account IDs, NPK/VPK material, and wallet
history remain redacted unless the evidence contract explicitly requires them.

The bootstrap already creates the three public lifecycle roles. Register each
role and fund the sender through the official FFI one operation at a time:

```bash
export BONDED_PROVISION_EVIDENCE=evidence/testnet/candidates/wallet-provisioning.json
for role in sender owner sink; do
  RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
  python3 tools/lez_wallet_provision.py --submit \
    --wallet-source "$BONDED_LEZ_SOURCE" \
    --wallet-home "$LEE_WALLET_HOME_DIR" \
    --evidence "$BONDED_PROVISION_EVIDENCE" \
    --role "$role" register
done
RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
python3 tools/lez_wallet_provision.py --submit \
  --wallet-source "$BONDED_LEZ_SOURCE" \
  --wallet-home "$LEE_WALLET_HOME_DIR" \
  --evidence "$BONDED_PROVISION_EVIDENCE" \
  --role sender fund
```

Each returned hash is journaled before finality polling, allowing the exact
operation to resume after a network interruption without resubmission. The
candidate records the canonical public-transaction byte hash, finalized block,
and authoritative before/after account state. Explorer validation remains a
separate promotion step. A process interruption during the submit call leaves an
explicit `submitting` entry and blocks automatic retry until wallet/sequencer
state is reconciled, preventing an ambiguous second funding claim.

After a polling timeout, inspect the exact journaled hash without loading the
wallet or resubmitting anything:

```bash
python3 tools/lez_candidate_status.py \
  --candidate "$BONDED_PROVISION_EVIDENCE" \
  --operation register:sender \
  --wait 1800
```

The read-only command distinguishes `verification-unavailable`,
`not-sequencer-included`, `sequencer-included`, `pending-indexer`, `disputed`,
and `finalized`. A timeout or unavailable service never authorizes a retry.

Once the provisioning journal itself is `finalized`, promote that exact hash
through the public explorer contract without opening the wallet:

```bash
python3 tools/lez_wallet_evidence.py \
  --candidate "$BONDED_PROVISION_EVIDENCE" \
  --operation register:sender \
  --verifier-commit "$(git rev-parse HEAD)" \
  --observer primary \
  --evidence evidence/testnet/wallet/register-sender.json
```

The promotion command refuses submitted or non-final journal entries, binds the
operation's exact official program and public accounts, and creates evidence
immutably. Run it later with a distinct observer and output path for the required
second observation.

## Program Deployment

Inspect the exact guest before authorization:

```bash
python3 tools/lez_wallet.py inspect-deployment \
  --elf build/lez/bonded_inbox.bin
```

Deployment requires two explicit gates and real-proof mode, even though the LEZ
deployment transaction itself is unsigned and contains no ZK receipt:

```bash
RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
BONDED_LEZ_SOURCE="$BONDED_LEZ_SOURCE" \
BONDED_LEZ_WALLET_HOME="$LEE_WALLET_HOME_DIR" \
scripts/deploy-lez-official-wallet.sh
```

The wrapper suppresses raw upstream output, rejects sensitive markers, checks
the returned hash against the serialization oracle, and writes only an
`official-wallet-sequencer-included` candidate. It never promotes that candidate
to verified evidence. Promotion requires `tools/lez_explorer.py reconcile` and
the complete Explorer Validation Contract.

## Standalone And Public Boundaries

Standalone integration tests use the same pinned wallet APIs and
`RISC0_DEV_MODE=0`; their transaction hashes and proof measurements demonstrate
construction compatibility but are not public-testnet evidence. Public evidence
must contain official explorer transaction and finalized block URLs. Failed
wallet/sequencer submissions and locally computed hashes never count as
successful transactions.

Fresh-browser supporting evidence is captured with
`tools/lez_explorer_capture.mjs`. It accepts only exact official transaction and
block URLs, uses a new temporary Chrome profile, asserts rendered identifiers,
rejects not-found pages, captures the full content dimensions, and records the
PNG and rendered-text SHA-256 values. It refuses to overwrite an evidence
directory and writes nothing unless browser shutdown and temporary-profile
cleanup also complete. Run its protocol validation with:

```bash
scripts/run-browser-evidence-tests.sh
```
