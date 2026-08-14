# Requirement Traceability

Status values are `planned`, `implemented`, `verified-local`, and
`verified-testnet`. No item is complete for submission before its evidence link
exists.

`verified-local` means checked through local tests, a pinned module build, or
static assets. It does not imply live upstream interoperability, an LEZ proof,
or a testnet result.

| ID | Requirement | Chunk | Verification | Status |
|---|---|---:|---|---|
| CORE-01 | Load beside wallet, storage, and messaging without upstream changes | 01, 04 | `nix build .#lib`, `nix build .#lgx` | verified-local |
| CORE-02 | Independent shielded NPK/ISK identity and funds | 04, 08 | LEZ integration/testnet evidence | planned |
| CORE-03 | Single-command headless deployment | 15 | `tests/test_cli.py` deploy/idempotency/restore | verified-local |
| CORE-04 | Encrypted owner channel from separate app | 06, 16 | Two `MessagingService` instances verify X25519/AES-GCM sealing, pinned Ed25519 identity, no wire plaintext, and replay rejection; the pinned Delivery client is bound, while live two-process transport remains pending | verified-local |
| CORE-05 | Per-transaction and period spending approval | 08 | `tests/test_services.cpp` spending cases | verified-local |
| SK-STO | `upload`, `download`, `list`, `share` | 07 | generated Storage client binding covers upload/download; local encrypted metadata/grant cases cover list/share; adapter suite, 21-skill suite, and `nix build .#lib` | verified-local |
| SK-MSG | `send`, `join`, `create_group` | 06 | generated Delivery client binding, send/subscribe/event adapter cases, 21-skill suite, and `nix build .#lib` | verified-local |
| SK-BC | `balance`, `send`, `history`, `query`, `call`, `deploy` | 08 | adapter and 21-skill runtime suites | verified-local |
| SK-A2A | `card`, `discover`, `task`, `subscribe`, `cancel` | 13 | `tests/test_second_half.cpp`, runtime suite | verified-local |
| SK-META | `skills`, `status`, `configure` | 14 | catalog union, signed configure tests | verified-local |
| A2A-01 | Signed cards and A2A lifecycle over Logos Messaging | 13 | signed-card and paid-task local lifecycle | verified-local |
| REL-01 | Recover from network interruption and restart | 03, 17 | `tests/test_main.cpp` verifies SIGKILL/WAL/outbox replay plus pending-bond settlement and receipt deduplication across two service restarts | verified-local |
| REL-02 | Unreachable owner cannot authorize spend | 08, 17 | approval timeout negative test | verified-local |
| REL-03 | Skill failures are isolated | 04, 14 | failing external skill followed by healthy dispatch | verified-local |
| PERF-01 | CU cost for every on-chain operation | 09, 20 | Dated CU report | planned |
| CI-01 | Standalone LEZ end-to-end CI | 18 | fast CI implemented; real environment remains conditional | implemented |
| CI-02 | Real proofs with `RISC0_DEV_MODE=0` | 09, 18, 21 | [`evidence/standalone/official-wallet-real-proof.json`](../../evidence/standalone/official-wallet-real-proof.json): 4/5 cases; private-transfer proof pending | implemented |
| UI-01 | Basecamp owner experience and assets | 16 | Runnable fixture preview, static asset checks, offscreen Qt instantiation, and `nix build .#basecamp-lgx`; live host test pending | verified-local |
| DEPLOY-01 | Three testnet agents: Messaging, Storage, Blockchain | 20 | Deployment evidence bundles | planned |
| DEMO-01 | Personal file vault | 21 | storage skill local demo/tests | verified-local |
| DEMO-02 | Paid skill marketplace/private pipeline | 21 | A2A paid-task local demo/tests | verified-local |
| DEMO-03 | On-chain event alerter | 21 | program/messaging skill composition; live event test pending | implemented |
| BOND-01 | Accepted returns full bond | 09, 10 | C++ and Rust lifecycle tests | verified-local |
| BOND-02 | Rejected goes to fixed sink, never owner | 09, 10 | C++ and Rust invariant tests | verified-local |
| BOND-03 | Expired and failed delivery return full bond | 09, 10 | C++ and Rust lifecycle tests | verified-local |
| BOND-04 | Classification alone cannot confiscate funds | 12 | triage and inbox adversarial tests | verified-local |
| BOND-05 | Real LEZ settlement guest and fail-closed publication | 09, 20A-20F | Corrected official-wallet deployment has two separated finalized observations and paired transaction/block captures; private-transfer real-proof and lifecycle calls remain pending | implemented |
| LEZ-01 | Release transactions use the pinned official wallet path | 20B | Corrected deployment and wallet provisioning use the pinned official path; standalone qualification remains 4/5 while the private-transfer proof runs | implemented |
| LEZ-02 | Settlement deployment passes the complete public explorer evidence gate | 20C, 20F | `settlement-program-deployment` row passes the offline gate with exact binary/program/transaction/block bindings and paired browser captures | implemented |
| LEZ-03 | Registration/funding transactions pass the public explorer evidence gate | 20B, 20E, 20F | All three `register-*` rows and `fund-sender` pass the offline evidence gate; whole-inventory live release audit remains pending | implemented |
| LEZ-04 | Acceptance, rejection, expiry, and delivery-failure transactions pass the public explorer gate | 20D, 20F | Eight lifecycle inventory rows | planned |
| LEZ-05 | Spending-control and paid-task transactions pass the public explorer gate | 20E, 20F | Three value-moving inventory rows | planned |
| INBOX-01 | Signed immutable policy and sender preflight | 05, 11 | signature/version/hash tests | verified-local |
| INBOX-02 | Trusted bypass, rate limits, one attachment | 07, 11 | intake and service tests | verified-local |
| PRIV-01 | No content, keys, attachments, or history in logs/on-chain | 05, 19 | telemetry redaction and asset tests | verified-local |
| DOC-01 | SDK, deployment, owner, A2A, security and operations docs | 22 | repository documentation set | implemented |
| SUB-01 | Narrated video, evidence, clean public release, draft submission | 21, 22 | Independent release audit | planned |
