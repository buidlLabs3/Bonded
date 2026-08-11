# Finding Log

| ID | Severity | Finding | Resolution |
|---|---|---|---|
| F-001 | high | Owner could be configured as rejection sink. | Policy and bond validation reject owner/sink equality. |
| F-002 | high | Classifier output could be mistaken for rejection authority. | Triage always quarantines model results; deterministic reasons are enumerated. |
| F-003 | high | Arbitrary bond IDs could orphan settlement. | Untrusted submissions require `bond:<message-id>`. |
| F-004 | medium | Configuration updates could race. | Owner signature and expected revision are mandatory. |
| F-005 | medium | Backup restore could traverse paths. | Absolute, parent, symlink, and hard-link members are rejected. |
| F-006 | medium | Adapter outage could grow queues indefinitely. | Bounded queues and circuit breaker primitives fail closed. |

External adapter and testnet qualification findings are tracked as release gates
in `docs/known-limitations.md`.
