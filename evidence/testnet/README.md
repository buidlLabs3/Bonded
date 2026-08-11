# Testnet Evidence

Place sanitized `inbox.json`, `vault.json`, and `settlement.json` evidence here
only after independently deploying each profile. Each record must include
`status: verified`, profile, network/version, NPK, Agent Card reference, program
ID where applicable, transaction/proof references, UTC timestamp, release commit,
and reproducible command. Never include ISKs, seeds, private keys, plaintext, or
credentials.

`scripts/evaluator-real.sh` fails closed while any verified record is absent.
