# LEZ Testnet Deployment

Bonded Inbox targets the public LEZ endpoint at
`https://testnet.lez.logos.co`. The guest dependencies are pinned to official
LEZ v0.2.4 commit `47eba256479f6f785acbd138834340703cd03401`.
On 2026-08-12 its built-in authenticated-transfer and privacy-circuit program
IDs matched the public endpoint. The preflight checks those exact IDs on every
run and refuses deployment if the network ABI has drifted.

The release at commit `3c3054f59358864ca3ce93578e6d874778f1e230` is published
as program `fb83bbb4c6140cb07e9a206d67e96a496bd395eed231e0f6158a672549e9a75c`.
The official explorer now resolves
[transaction `d033...`](https://explorer.testnet.lez.logos.co/transaction/d033cfe9a59a97824711f2a4d3df571281adc739e196cba1a7cf2264958298ad)
as a Program Deployment Transaction and shows it in finalized
[block 4035](https://explorer.testnet.lez.logos.co/block/4035), hash
`603a76b3c88c4a611906624ce6a347c93108335be3f27dd6d03a662250d8f142`.
The reconciled evidence is `evidence/testnet/settlement-program.json`; the
sequencer-only artifact originally created at submission time is preserved in
`evidence/testnet/history/settlement-program-sequencer-included-20260812.json`.
See `docs/deployment/lez-testnet-reconciliation.md` for the observed service
tips, overlapping canonical blocks, and historical classification.

## Program Publication

Prerequisites are Rust, Docker, `cargo-risczero`, `r0vm`, Git, and initial
network access for locked dependencies and the RISC Zero builder image. The
deployment transaction is unsigned and requires no faucet balance, but the
official CLI still requires initialized wallet storage; protect its generated
recovery phrase as described below.

The release submission route is the pinned official wallet documented in
`docs/deployment/lez-wallet.md`. The direct encoder below remains available for
serialization conformance and historical diagnostics only; it is not authorized
to create new release evidence.

```bash
python3 tools/lez_testnet.py preflight
RISC0_DEV_MODE=0 scripts/build-lez-program.sh
```

The build compiles `programs/bonded-inbox/lez-guest`, verifies the host bond
model, computes the RISC Zero image ID, and copies the ignored ELF to
`build/lez/bonded_inbox.bin`. Inspect and commit the exact source before sending
the deployment transaction. Then follow the secret-safe wallet setup in
`docs/deployment/lez-wallet.md` and run:

```bash
RISC0_DEV_MODE=0 BONDED_LEZ_SUBMIT=YES \
BONDED_LEZ_SOURCE=/absolute/path/to/logos-execution-zone \
BONDED_LEZ_WALLET_HOME=/absolute/private/path/to/wallet \
scripts/deploy-lez-testnet.sh
```

The official wallet adapter:

- requires HTTPS and rejects endpoint credentials;
- requires `RISC0_DEV_MODE=0`;
- verifies an unmodified official v0.2.4 checkout and release binary;
- validates the RISC Zero program image and records its digest and size;
- submits through the official `wallet deploy-program` command;
- checks the returned hash against an independent serialization oracle;
- waits for block inclusion and writes no evidence until inclusion succeeds.

Direct submission writes only an `official-wallet-sequencer-included` candidate
under `evidence/testnet/candidates/`; it can never create successful public evidence.
After the official explorer indexes and finalizes the exact transaction, run the
read-only canonical verifier with the current full verifier commit:

```bash
python3 tools/lez_explorer.py reconcile \
  --verifier-commit "$(git rev-parse HEAD)" \
  --observer primary-release-audit \
  --evidence evidence/testnet/deployment/settlement-program-primary.json
```

Evidence paths are immutable. Use a new observer-specific path for each
reconciliation; the verifier refuses to overwrite any prior observation.

A successful verifier result contains hashes, IDs, block/finality, exact public
URLs, source revisions, confirmation depth, and a multi-block canonical-chain
comparison. It identifies a program component, not an agent profile. The ELF
stays ignored because it is reproducible from the release commit and lockfile.
A network error, missing hydration resource, not-found page, pending block,
transaction mismatch, block mismatch, bytecode mismatch, or fewer than three
matching finalized overlap blocks exits nonzero and cannot create passing data.

The release wrapper does not accept a sequencer override. `BONDED_LEZ_ELF` and
`BONDED_LEZ_EVIDENCE` may be changed only for an explicitly reviewed run. The
lower-level raw RPC tool is retained solely for conformance tests and historical
diagnostics; no release script invokes it.

## Agent Profiles

Publishing the program installs settlement logic but does not create any agent,
account, or private execution proof. The three LP-0008 profiles remain separate:

| Profile | Category | Remaining live prerequisite |
|---|---|---|
| Inbox | Messaging | Official Logos Messaging adapter, owner key, Core process |
| Vault | Storage | Official Logos Storage adapter, owner key, Core process |
| Settlement | Blockchain | Shielded wallet/Core adapter, funded NPK/ISK account, real program calls |

Each deployment needs its own shielded identity and data directory. Do not reuse
a seed or commit credentials. The current official Logos wallet module lacks the
required shielded LEZ API, so the repository intentionally does not manufacture
fake profile evidence from its in-memory adapters.

## Verification Boundary

Program evidence verifies only that the deterministic guest ELF was accepted in
a public LEZ block. Full project completion additionally requires:

- initializing and settling acceptance, rejection, expiry, and delivery-failure
  bonds through the shielded wallet path with real proofs;
- collecting CU measurements for every operation;
- deploying and operating all three Core profiles independently;
- running encrypted Messaging, Storage, A2A, Basecamp, and recovery flows against
  official modules;
- passing `scripts/evaluator-real.sh` with all three sanitized evidence bundles.
