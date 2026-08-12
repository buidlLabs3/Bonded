# Testnet Evidence

`settlement-program.json` is the current public program-publication record. It is
valid only while `tools/lez_explorer.py reconcile` returns `status: finalized`
for the exact transaction and block. The original sequencer-only artifact is
retained byte-for-byte under `history/` for auditability.

The `settlement-program-deployment` and `register-sender` inventory rows
currently pass the offline gate using primary and independently confirmed
observations plus paired browser captures. Those two passing rows do not qualify
the remaining wallet, lifecycle, spending-control, or paid-task rows. The
unindexed `register-sender-independent.json` observation is retained as history:
it agreed with the canonical transaction but was only one finalized block after
the primary observation, so it is not used to meet the three-block separation
rule.

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

`python3 tools/traceability_gate.py` is part of ordinary CI. While no requirement
claims `verified-testnet`, it validates the matrix vocabulary and uniqueness. If
any row makes that claim, the command also requires this entire offline evidence
inventory to pass; a single transaction artifact cannot promote a requirement.

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
