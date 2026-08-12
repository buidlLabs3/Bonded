# LEZ Testnet Reconciliation - 2026-08-12

## Outcome

The earlier `Transaction not found` / `Block not found` observation is resolved.
At `2026-08-12T08:27:21Z`, committed verifier
`e4a33e36cfce8b9e8547ee4aa8612d0b948dd20b` independently read the official
sequencer and explorer and returned `finalized` for transaction
`d033cfe9a59a97824711f2a4d3df571281adc739e196cba1a7cf2264958298ad`.

The exact explorer transaction is a Program Deployment Transaction. Its decoded
bytecode is 370032 bytes with SHA-256
`8bca02c366c3261474e51818d8b2a95a441121c85bc57acf7bcc62dc8566182a`.
The exact explorer block is finalized block 4035 with hash
`603a76b3c88c4a611906624ce6a347c93108335be3f27dd6d03a662250d8f142`,
and its transaction list contains the exact deployment hash.

## Time-Correlated Observation

| Surface | Observation |
|---|---|
| Sequencer | Healthy; channel `0101...0101`; head 4153 was Pending |
| Sequencer finalized boundary | Block 4094 was Finalized |
| Explorer/indexer finalized tip | Block 4094, hash `931395...0812`, Finalized |
| Candidate confirmation depth | 59 finalized explorer blocks |
| Public indexer RPC/status | Not exposed/discovered; explorer SSR is the public observation surface |

Canonical overlap checks passed at three consecutive finalized heights:

| Block | Matching sequencer/explorer hash |
|---:|---|
| 4094 | `931395904036d933abb60707cd8059686eefecd0ec5b8c7d5adf268e6eac0812` |
| 4093 | `47bbd2c21b07e70a88461e5d52c041ac4afad9c7b35128d5db643f7212fbec3f` |
| 4092 | `3aaba9bd44c522a150b393577289cad874dff2793c740dc8595993a6d2d38a04` |

## Classification

There is no later evidence of a divergent chain, reset, pruning, wrong endpoint,
or invalid transaction encoding. The candidate block and three later finalized
blocks match exactly across services. The earlier failure is classified as
resolved `pending-indexer-or-finality`: the contemporaneous sequencer finality
and private indexer sync status were not captured, so choosing one cause would be
an unsupported inference.

Future observations use a bounded policy: require three confirmation blocks and
scan up to 32 blocks beyond the explorer finalized tip. A transaction stays
`pending-indexer` while the explorer is below its block. Any mismatch after the
explorer reaches the block is `disputed` and blocks downstream testnet claims.

## Reproduction

```bash
python3 tools/lez_explorer.py reconcile \
  --verifier-commit "$(git rev-parse HEAD)"
```

The verifier rejects HTML error/loading/not-found content, mismatched block and
transaction identifiers, wrong type, pending finality, bytecode differences,
insufficient confirmation depth, and canonical overlap disagreement. See
`docs/deployment/lez-incident-template.md` if a later check fails.
