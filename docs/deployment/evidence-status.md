# LEZ Evidence Status Vocabulary

These statuses are ordered observations, not synonyms. Tooling must only advance
a record when the evidence for the next state exists.

| Status | Meaning | Satisfies a public transaction gate |
|---|---|---|
| `built-local` | Artifact built and hashed locally | No |
| `submitted` | An official client accepted the serialized transaction | No |
| `sequencer-included` | Sequencer returns the exact transaction and block | No |
| `pending-indexer` | Sequencer inclusion exists but explorer/indexer is behind the block | No |
| `indexer-seen` | Indexer data contains the exact transaction, but rendering/finality/confirmation checks remain | No |
| `explorer-validated` | Exact transaction and block pages render and cross-check successfully | Yes, when finality is not required |
| `finalized` | Explorer-validated transaction is in a matching finalized block and has the required confirmation depth | Yes |
| `disputed` | A previously claimed identifier disappeared or conflicts across services | No |
| `failed` | Submission or validation failed terminally | No |
| `verification-unavailable` | Required official service could not be reached or parsed | No |

HTTP success alone never advances the state: the official explorer can return
HTTP 200 while rendering an error. Screenshots support a machine-readable record
but cannot replace it. A newer failed recheck demotes an existing passing record
to `disputed` until canonical state is reconciled.

The release inventory is `evidence/testnet/required-evidence.json`. Run
`scripts/verify-lez-evidence.sh` to repeat exact transaction and block lookups
without submitting anything. The gate requires two immutable finalized
observations and both clean-browser screenshots for every row. A network outage
is a failed qualification attempt (`verification-unavailable` in an archived
audit), never permission to reuse an earlier passing result.
