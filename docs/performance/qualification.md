# Performance And CU Qualification

`tools/benchmark_local.py` records local CLI deployment and status latency with
system and Python versions. These figures exercise local adapters only and are
not LEZ performance evidence. Queue, attachment, A2A concurrency, reconnect,
settlement, and proof benchmarks require the pinned multi-process stack.

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
