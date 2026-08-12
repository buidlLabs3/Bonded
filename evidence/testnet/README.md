# Testnet Evidence

`settlement-program.json` is the current public program-publication record. It is
valid only while `tools/lez_explorer.py reconcile` returns `status: finalized`
for the exact transaction and block. The original sequencer-only artifact is
retained byte-for-byte under `history/` for auditability.

Place sanitized `inbox.json`, `vault.json`, and `settlement.json` profile evidence
here only after independently deploying each profile. Each record must include a
status from `docs/deployment/evidence-status.md`, profile, network/version, NPK,
Agent Card reference, program ID where applicable, transaction/proof references,
UTC timestamp, release commit, and reproducible command.

Never include ISKs, seeds, private keys, mnemonic phrases, plaintext, credentials,
private proofs, encrypted payloads, or complete private wallet history. Explorer
HTML is parsed in memory and is not committed because deployment bytecode can be
reproduced from the pinned source.

`scripts/evaluator-real.sh` fails closed while any verified record is absent.
