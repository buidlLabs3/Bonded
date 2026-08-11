# Requirement Traceability

Status values are `planned`, `implemented`, `verified-local`, and
`verified-testnet`. No item is complete for submission before its evidence link
exists.

| ID | Requirement | Chunk | Verification | Status |
|---|---|---:|---|---|
| CORE-01 | Load beside wallet, storage, and messaging without upstream changes | 01, 04 | Logos Core load integration | planned |
| CORE-02 | Independent shielded NPK/ISK identity and funds | 04, 08 | LEZ integration/testnet evidence | planned |
| CORE-03 | Single-command headless deployment | 15 | Clean-machine deployment test | planned |
| CORE-04 | Encrypted owner channel from separate app | 06, 16 | Two-instance integration | planned |
| CORE-05 | Per-transaction and period spending approval | 08 | Concurrency/restart/timeout tests | planned |
| SK-STO | `upload`, `download`, `list`, `share` | 07 | Storage contract/integration tests | planned |
| SK-MSG | `send`, `join`, `create_group` | 06 | Delivery contract/integration tests | planned |
| SK-BC | `balance`, `send`, `history`, `query`, `call`, `deploy` | 08 | LEZ contract/integration tests | planned |
| SK-A2A | `card`, `discover`, `task`, `subscribe`, `cancel` | 13 | A2A conformance/paid-task test | planned |
| SK-META | `skills`, `status`, `configure` | 14 | Generated conformance suite | planned |
| A2A-01 | Signed cards and A2A lifecycle over Logos Messaging | 13 | Schema/interoperability suite | planned |
| REL-01 | Recover from network interruption and restart | 03, 17 | Fault-injection suite | planned |
| REL-02 | Unreachable owner cannot authorize spend | 08, 17 | Retry/timeout negative test | planned |
| REL-03 | Skill failures are isolated | 04, 14 | Crash/timeout sample skill | planned |
| PERF-01 | CU cost for every on-chain operation | 09, 20 | Dated CU report | planned |
| CI-01 | Standalone LEZ end-to-end CI | 18 | Green default-branch workflow | planned |
| CI-02 | Real proofs with `RISC0_DEV_MODE=0` | 09, 18, 21 | Asserted logs/video | planned |
| UI-01 | Basecamp owner experience and assets | 16 | QML integration/accessibility tests | planned |
| DEPLOY-01 | Three testnet agents: Messaging, Storage, Blockchain | 20 | Deployment evidence bundles | planned |
| DEMO-01 | Personal file vault | 21 | Script/video/evidence | planned |
| DEMO-02 | Paid skill marketplace/private pipeline | 21 | Script/video/evidence | planned |
| DEMO-03 | On-chain event alerter | 21 | Script/video/evidence | planned |
| BOND-01 | Accepted returns full bond | 09, 10 | Program and lifecycle tests | planned |
| BOND-02 | Rejected goes to fixed sink, never owner | 09, 10 | Invariant/adversarial tests | planned |
| BOND-03 | Expired and failed delivery return full bond | 09, 10 | Program and lifecycle tests | planned |
| BOND-04 | Classification alone cannot confiscate funds | 12 | Authorization/adversarial tests | planned |
| INBOX-01 | Signed immutable policy and sender preflight | 05, 11 | Signature/version tests | planned |
| INBOX-02 | Trusted bypass, rate limits, one attachment | 07, 11 | Intake integration tests | planned |
| PRIV-01 | No content, keys, attachments, or history in logs/on-chain | 05, 19 | Redaction/privacy tests | planned |
| DOC-01 | SDK, deployment, owner, A2A, security and operations docs | 22 | Clean-clone documentation audit | planned |
| SUB-01 | Narrated video, evidence, clean public release, draft submission | 21, 22 | Independent release audit | planned |
