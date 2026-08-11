# Demo And Narration Runbook

Run local evidence inside `nix develop`:

```bash
nix develop --command bash scripts/demo-local.sh
```

The native suites demonstrate bonded acceptance/refund, explicit rejection to a
fixed sink, trusted bypass, attachment encryption, replay protection, spending
approval/timeout, signed messaging, A2A discovery/payment lifecycle, triage
safety, and recovery primitives. Python suites demonstrate deployment,
backup/restore, upgrade/rollback, guarded teardown, and Basecamp assets.

The three named use cases are:

1. **Personal file vault**: encrypted upload, authenticated download, metadata
   list, and expiring share grant.
2. **Paid skill marketplace / privacy-preserving agent pipeline**: discover a
   signed processing card, lock its advertised price, complete or cancel, and
   release/refund idempotently.
3. **On-chain event alerter**: query/call events are correlated to a signed
   private owner-channel notification without on-chain content.

For the narrated release recording, show architecture, exact upstream pins,
three independently identified testnet profiles, signed cards, message and
attachment privacy, above-limit approval timeout, restart recovery, CU receipts,
and visible `RISC0_DEV_MODE=0` proof generation. No video or testnet proof is
claimed in this repository yet.
