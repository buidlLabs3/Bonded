# Finding Log

| ID | Severity | Finding | Resolution |
|---|---|---|---|
| F-001 | high | Owner could be configured as rejection sink. | Policy and bond validation reject owner/sink equality. |
| F-002 | high | Classifier output could be mistaken for rejection authority. | Triage always quarantines model results; deterministic reasons are enumerated. |
| F-003 | high | Arbitrary bond IDs could orphan settlement. | Untrusted submissions require `bond:<message-id>`. |
| F-004 | medium | Configuration updates could race. | Owner signature and expected revision are mandatory. |
| F-005 | medium | Backup restore could traverse paths. | Absolute, parent, symlink, and hard-link members are rejected. |
| F-006 | medium | Adapter outage could grow queues indefinitely. | Bounded queues and circuit breaker primitives fail closed. |
| F-007 | high | Signed v1 messaging envelopes exposed owner-channel plaintext to the transport adapter. | V1 is rejected. V2 removes plaintext from the wire schema; fresh X25519 + AES-256-GCM sealing precedes Ed25519 signing, and two-instance tests reject wrong keys/identities/tampering/replay. |
| F-008 | high | Bond records existed only in process memory, so a restart could leave a durable pending message unable to settle. | Bonds and outcomes now persist in SQLite. Submission and terminal settlement resume from durable state, and stable outbox keys prevent duplicate intake or receipt events across restarts. |

External adapter and testnet qualification findings are tracked as release gates
in `docs/known-limitations.md`.
