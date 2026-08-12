# LEZ Testnet Deployment

Bonded Inbox targets the public LEZ endpoint at
`https://testnet.lez.logos.co`. The guest dependencies are pinned to official
LEZ v0.2.4 commit `47eba256479f6f785acbd138834340703cd03401`.
On 2026-08-12 its built-in authenticated-transfer and privacy-circuit program
IDs matched the public endpoint. The preflight checks those exact IDs on every
run and refuses deployment if the network ABI has drifted.

## Program Publication

Prerequisites are Rust, Docker, `cargo-risczero`, `r0vm`, Git, and initial
network access for locked dependencies and the RISC Zero builder image.
No seed phrase, private key, or faucet balance is needed for LEZ program
publication.

```bash
python3 tools/lez_testnet.py preflight
RISC0_DEV_MODE=0 scripts/build-lez-program.sh
```

The build compiles `programs/bonded-inbox/lez-guest`, verifies the host bond
model, computes the RISC Zero image ID, and copies the ignored ELF to
`build/lez/bonded_inbox.bin`. Inspect and commit the exact source before sending
the deployment transaction. Then run:

```bash
RISC0_DEV_MODE=0 scripts/deploy-lez-testnet.sh
```

The deployment client:

- requires HTTPS and rejects endpoint credentials;
- requires `RISC0_DEV_MODE=0`;
- verifies the live built-in program IDs against v0.2.4;
- validates the RISC Zero program image and computes its ID with `r0vm`;
- Borsh-encodes `LeeTransaction::ProgramDeployment` exactly as the pinned source;
- computes and checks the expected transaction hash;
- checks for a prior identical transaction before sending;
- waits for block inclusion and writes no evidence until inclusion succeeds.

Successful public evidence is written to
`evidence/testnet/settlement-program.json`. It contains hashes, IDs, block,
endpoint, source revisions, and timestamp only. It identifies a program
component, not an agent profile. The ELF stays ignored because it is
reproducible from the release commit and lockfile.

Override `LEZ_SEQUENCER_URL`, `BONDED_LEZ_ELF`, or `BONDED_LEZ_EVIDENCE` only
for an explicitly reviewed environment. Plain HTTP requires the Python client's
`--allow-http` switch and is intended only for a loopback standalone sequencer;
the public deploy wrapper never enables it.

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
