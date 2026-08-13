# Performance And CU Qualification

`tools/benchmark_local.py` compiles the native workload runner and emits a
versioned, source-bound JSON report for CLI startup/status, bounded queues, spam
bursts, one-MiB attachment round-trips, concurrent A2A tasks, durable SQLite
recovery, local settlement, and peak RSS. It exits nonzero if any supported
limit fails:

```bash
python3 tools/benchmark_local.py \
  --output evidence/performance/local-$(date -u +%Y%m%dT%H%M%SZ).json
```

The output is local-test-adapter qualification only, not LEZ performance or CU
evidence. The benchmark storage double accepts a complete in-memory object, so
the attachment measurement is explicitly a bounded round-trip, not streaming.
Official multi-process adapter, real-proof, and testnet receipt measurements
remain separate release gates.

| Supported local limit | Required result |
|---|---:|
| CLI deploy p95 | <= 1,500 ms |
| CLI status p95 | <= 1,500 ms |
| Bounded queue | >= 20,000 operations/s with overflow rejected |
| Spam burst | >= 20,000 attempts/s with the exact configured limit enforced |
| One-MiB attachment round-trip | >= 5 MiB/s |
| Concurrent A2A completion | >= 50 tasks/s |
| Durable reopen and verification | <= 500 ms |
| Local bond lock + settle | >= 5,000 operations/s |
| Native peak RSS | <= 262,144 KiB |

Supported defensive defaults are a one MiB attachment limit in the example
policy, one attachment per message, positive bounded rate windows, 300-second
default message-envelope expiry, 1,000-ms classifier timeout, bounded queues,
100 LEZ per transaction, 500 LEZ per period, and 3,600-second approval timeout.

| On-chain operation | CU | Network/version/date | Evidence |
|---|---:|---|---|
| transfer | not measured | pending testnet | required |
| program query | not measured | pending testnet | required |
| program call | not measured | pending testnet | required |
| program deploy | not measured | pending testnet | required |
| bond lock | not measured | pending testnet | required |
| bond accept/refund | not measured | pending testnet | required |
| bond reject/sink | not measured | pending testnet | required |
| bond expiry/failure refund | not measured | pending testnet | required |
| A2A escrow lock/release/refund | not measured | pending testnet | required |

Missing values are deliberate fail-closed release gates. They must be measured
from transaction/proof receipts on the exact release network; estimates are not
substitutes.
