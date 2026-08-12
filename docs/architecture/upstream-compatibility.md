# Upstream Compatibility Matrix

Validated on 2026-08-12. Revisions are immutable inputs; upgrades require a
compatibility test and an ADR update.

| Component | Revision | Integration decision |
|---|---|---|
| Logos Module Builder | `7fbb9420a3fe8ce03a140f0df84a0cc8463bc6dc` | Universal C++ module interface; Qt glue generated from the implementation header |
| Logos Storage Module | `e0db835de379f47bae7fccc3032056d99af973bb` | Isolate behind `StorageAdapter`; upstream metadata version 2.1.0 |
| Logos Delivery Module | `3f0f2d8b202f427a96179407bbf18b449935da7c` | Isolate behind `MessagingAdapter`; upstream metadata version 0.2.0 |
| Logos Wallet Module | `f6f9c160410db824531f42dd8b27ccecd39ca589` | Not suitable for LEZ shielded accounts: current API is an explicitly WIP Ethereum transaction wrapper |
| Logos Basecamp | `cee212ffa82187d05dc704d78c1184289e8fca1f` | Package UI as an `.lgx`; test with isolated `--user-dir` instances |
| Logos Execution Zone testnet v0.2.4 | `47eba256479f6f785acbd138834340703cd03401` | Exact source/ABI whose authenticated-transfer and privacy-circuit IDs match `https://testnet.lez.logos.co` on 2026-08-12 |
| SPEL | `0cb7e0980535af619482cf1c823f4d394b3ebd61` | Reassess after bond-program spike; no dependency until it materially reduces proof/program risk |
| A2A | `4f82944ddf6821b9d19bc4aa03a59acd8a09a4ce` | Bind the `lf.a2a.v1` schema over Logos Messaging; use current submitted/working/input-required/auth-required/terminal states |
| Lambda Prize | `de7c5ae91b90c585a15672bb43bfc04daab1732b` | LP-0008 is the acceptance baseline and is marked open at this revision |

## Known Compatibility Gap

LP-0008 requires a shielded LEZ NPK/ISK account, but the current official
`logos-wallet-module` does not expose that API. Bonded Inbox therefore keeps a
strict `WalletAdapter` boundary and targets the wallet FFI in the pinned LEZ
repository for the initial implementation. The adapter must be replaced with a
Core module call when an official shielded LEZ wallet module is published. This
gap blocks a claim of final testnet conformance until the real adapter and proof
path pass Chunk 20.

## Upgrade Policy

Update one upstream at a time. Regenerate the module contract, run unit and
integration suites, perform the `RISC0_DEV_MODE=0` path, and record any schema,
ABI, privacy, or CU impact before changing a pin.
