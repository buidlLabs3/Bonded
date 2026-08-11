# First-Half Checkpoint: Chunks 00-11

Date: 2026-08-11

This checkpoint records what is implemented and what remains blocked on real
upstream/testnet integration. It is not a claim that Chunks 00-11 satisfy their
final testnet exit criteria.

## Locally Verified

- Pinned Logos module builder and full transitive `flake.lock`.
- Generated universal module contract: 6 methods and 2 typed events.
- Buildable `bonded_inbox_plugin.so` and `.lgx` package.
- Monotonic message state transitions and optimistic revision checks.
- Bond acceptance/refund, explicit rejection/fixed sink, expiry, failed-delivery
  outcome, duplicate settlement, and conflicting settlement invariants.
- Signed/versioned policies that prohibit the owner as rejection sink.
- Ed25519 signatures and AES-256-GCM authenticated encryption via OpenSSL EVP.
- SQLite migrations, idempotent intake, processed-event deduplication, and
  outbox acknowledgement.
- Profile allowlists and typed skill registration/dispatch.
- Signed messaging envelopes, network/expiry checks, and replay deduplication.
- Encrypted content-addressed storage boundary and expiring grants.
- Per-transaction/period spending policy, above-limit approval, denial, expiry,
  and duplicate-approval behavior.
- Trusted-contact bypass, attachment policy enforcement, and sender rate limits.
- Signed private receipts and an allocation-free Rust bond program core.

## Verification Results

- Lifecycle/security/persistence suite: 7 of 7 groups passed.
- Service boundary suite: 6 of 6 groups passed.
- Rust bond program: 4 of 4 tests passed.
- Logos code generation: passed.
- Logos Core shared plugin build: passed.
- Development `.lgx` package build: passed.

## Open Integration Gates

- The official `logos-wallet-module` revision inspected in Chunk 00 exposes a
  WIP Ethereum API, not the shielded LEZ NPK/ISK API required by LP-0008.
- Messaging and Storage behavior is verified behind adapters, but the actual
  pinned Logos modules must replace the deterministic memory adapters.
- The Rust bond core must be wrapped in the pinned LEZ program ABI, deployed,
  and executed against a standalone sequencer with `RISC0_DEV_MODE=0`.
- Pending bonds are locally represented; chain reconciliation and confirmation
  tracking require the real LEZ adapter.
- Separate owner/Basecamp and headless multi-process tests remain pending.

No test double output may be placed in `evidence/testnet` or cited in the Lambda
Prize submission.
