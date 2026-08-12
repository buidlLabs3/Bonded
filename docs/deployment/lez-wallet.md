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
and deleted. Output remains a sequencer-finalized candidate until the explorer
gate independently promotes it.

## Secret-Safe Wallet Setup

Create a dedicated testnet-only wallet directory outside the repository. Set
its directory mode to `0700`. The pinned v0.2.4 wallet does not encrypt its
storage at rest, so directory and file permissions are a hard security boundary.
The bootstrap wrapper calls the official FFI, writes the recovery phrase without
printing it, and creates distinct sender, owner, and sink public identities.
Use a new absolute path and immediately transfer the recovery phrase from the
generated `0600` file into an approved secret manager.

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
