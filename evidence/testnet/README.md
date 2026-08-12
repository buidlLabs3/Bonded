# Testnet Evidence

`settlement-program.json` is the current public program-publication record. It is
valid only while `tools/lez_explorer.py reconcile` returns `status: finalized`
for the exact transaction and block. The original sequencer-only artifact is
retained byte-for-byte under `history/` for auditability.

`required-evidence.json` is the complete public transaction inventory for the
release gate. Each row requires two distinct finalized machine observations,
separated by at least the configured confirmation depth, plus fresh-browser
transaction and block screenshots. Run the read-only live gate with:

```sh
scripts/verify-lez-evidence.sh
```

The command submits nothing. Missing artifacts, unavailable official services,
duplicate transaction reuse, leaked sensitive fields, stale observations, and
identifier mismatches all fail qualification. `evidence.schema.json` documents
the common finalized observation contract; the gate enforces the cross-file and
live-network rules that JSON Schema cannot express.

Place sanitized `inbox.json`, `vault.json`, and `settlement.json` profile evidence
here only after independently deploying each profile. Each record must include a
status from `docs/deployment/evidence-status.md`, profile, network/version, NPK,
Agent Card reference, program ID where applicable, transaction/proof references,
UTC timestamp, release commit, and reproducible command.

Never include ISKs, seeds, private keys, mnemonic phrases, plaintext, credentials,
private proofs, encrypted payloads, or complete private wallet history. Explorer
HTML is parsed in memory and is not committed because deployment bytecode can be
reproduced from the pinned source.

`scripts/evaluator-real.sh` fails closed while any inventory row is absent or
does not pass a fresh live recheck.
