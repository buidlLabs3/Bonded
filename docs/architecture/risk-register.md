# Risk Register

| Risk | Impact | Mitigation / gate |
|---|---|---|
| No stable shielded LEZ Core wallet module | Blocks full LP-0008 conformance | Adapter targets pinned LEZ wallet FFI; require real testnet evidence before completion |
| Rapid upstream ABI/schema changes | Build or protocol breakage | Immutable pins, compatibility tests, one-at-a-time upgrades |
| Real proofs exceed hosted CI resources | Required CI path cannot finish | Benchmark early, use cache, provision a dedicated release runner; never substitute dev mode |
| Owner or retry race double-settles a bond | Financial loss | Program-level terminal state plus local idempotency and reconciliation |
| Classifier causes confiscation | Unfair loss and prompt-injection path | Model is advisory; signed owner action or deterministic violation is mandatory |
| Bond discourages low-balance senders | Economic exclusion | Small defaults, trusted bypass, and published bond-free emergency channel |
| Topic/telemetry metadata leaks relationships | Privacy loss | Opaque topics, bounded metrics, redaction tests, transport threat model |
| Storage grant or temp file leaks plaintext | Confidentiality loss | AEAD, expiring grants, safe temp handling, negative authorization tests |
