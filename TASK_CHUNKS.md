# Bonded Inbox: Project Execution Chunks

## Purpose

This file is the execution plan for taking the current Bonded Inbox repository
to a complete LP-0008 submission. It is a plan and acceptance contract, not a
claim that listed work is complete. Existing implementation and evidence must be
re-evaluated against every exit criterion below; an artifact created earlier is
not grandfathered into a stronger verification state.

The product brief is `../README.md`. The external acceptance baseline is the
official [LP-0008 specification](https://github.com/logos-co/lambda-prize/blob/master/prizes/LP-0008.md).
Because the Logos stack is evolving, Chunk 00 must pin verified upstream
revisions and replace any provisional API assumptions before implementation.

## Completion Contract

The project is complete only when all of the following are true:

- Bonded Inbox implements bonded first-contact messaging without placing message
  content, attachments, wallet history, or private identity data on-chain.
- A dynamically loadable Logos Core module runs beside wallet, storage, and
  messaging modules without modifying those upstream modules.
- All LP-0008 default Storage, Messaging, Blockchain, A2A, and Meta skills are
  implemented and documented.
- Inbox, Vault, and Settlement profiles are reproducibly deployed on LEZ testnet
  as the required Messaging, Storage, and Blockchain agent deployments, and
  every claimed on-chain operation resolves by exact hash on the official LEZ
  testnet explorer and in its finalized block.
- Spending controls, owner approval, encrypted owner communication, recovery,
  failure isolation, replay protection, and idempotent settlement are verified.
- At least three LP-0008 illustrative use cases and all Bonded Inbox demo flows
  run end-to-end.
- CI is green, including an end-to-end standalone LEZ sequencer run with real
  proof generation and `RISC0_DEV_MODE=0`.
- The public repository, documentation, testnet evidence, narrated video, and
  Lambda Prize submission are evaluator-ready.

## Architecture Baseline

Validate this baseline in Chunk 00, then record changes in ADRs:

- **Implementation:** C++/Qt 6 Logos Core module built with CMake and Nix through
  `logos-module-builder`'s universal module interface.
- **Runtime:** one agent runtime with profile-based capabilities and least
  privilege: Inbox/Messaging, Vault/Storage, and Settlement/Blockchain.
- **UI:** a Basecamp-compatible QML module for inbox, policy, review, approval,
  and receipt views; the headless runtime remains fully operable by CLI.
- **Transport:** signed A2A envelopes over encrypted Logos Messaging topics.
- **Storage:** encrypted message metadata and workflow state locally; encrypted
  attachment ciphertext in Logos Storage; only bond state and settlement go to
  LEZ.
- **Settlement:** a replay-protected LEZ program or the verified equivalent
  upstream primitive controls bond locking, refund, rejection settlement,
  expiry, and failed-delivery refund.
- **Inference:** a pluggable local classifier. Model output may accept or
  quarantine, but never confiscate a bond without an explicit owner action or a
  deterministic policy violation.
- **Persistence:** transactional durable state with an outbox/inbox pattern and
  explicit migrations. No key or plaintext is written to logs.
- **Licensing:** MIT or Apache-2.0, confirmed before third-party code is added.

## Profile-to-Prize Mapping

| Product profile | LP-0008 deployment category | Primary responsibilities |
|---|---|---|
| Inbox Agent | Messaging | Agent Card and policy publication, encrypted intake, owner channel, trusted-contact and rate-limit enforcement |
| Vault Agent | Storage | Attachment encryption, upload/download/list/share, temporary grants, paid processing handoff |
| Settlement Agent | Blockchain | Shielded wallet, bond verification and settlement, program operations, receipts, spending approval |

All profiles use the same runtime and skill SDK. Capability allowlists, keys,
configuration, and persistent state are separate per deployment.

## Execution Rules

- Execute chunks in dependency order. A chunk is complete only when every exit
  criterion is supported by committed code, tests, or evidence.
- Keep a live traceability matrix from every local and LP-0008 requirement to a
  test, document, demo step, and evidence artifact.
- Pin all upstream inputs. An unpinned branch reference is not release-ready.
- Prefer upstream module interfaces and generated Logos module glue over forks
  or local patches. Any unavoidable patch needs an ADR and upstream issue link.
- Use test doubles only below integration boundaries. Release evidence must use
  the actual Logos modules and actual LEZ proof generation.
- Treat sequencer submission, sequencer RPC inclusion, indexer ingestion,
  explorer rendering, and finalization as different states. Never collapse them
  into one `verified` status.
- Do not call a LEZ transaction testnet-verified from a locally computed hash,
  a `sendTransaction` response, a sequencer `getTransaction` response, a block
  number, a screenshot, or a private receipt alone.
- Never use real-value or production credentials in development, tests, CI, or
  recorded demos.
- Do not mark a financial path complete until retry, duplicate, crash, and
  partial-failure behavior is tested.

## LEZ Explorer Validation Contract

The official public explorer for this plan is
`https://explorer.testnet.lez.logos.co`. Its pinned v0.2.4 route contract is:

- `/transaction/<64-character-hex-hash>` for transaction details;
- `/block/<numeric-block-id>` for the containing block;
- `/account/<account-id>` for public account state/history when the operation
  legitimately exposes a public account.

A transaction may be labeled `explorer-validated` only when all of these checks
pass against the same network observation:

1. The official sequencer returns the exact transaction hash and containing
   block, and the returned transaction bytes/type match the submitted operation.
2. The official explorer/indexer has caught up to at least the containing block;
   an indexer that is behind, stalled, or connected to a different chain makes
   the result pending, not passing.
3. A fresh unauthenticated browser session opens the exact transaction URL and
   renders the expected hash and transaction type without `Transaction not
   found`, an error page, or an unresolved loading state.
4. The exact block URL renders the expected block ID, contains the transaction
   hash, and reports the upstream finality state required by the pinned release.
5. The transaction remains resolvable after the configured confirmation depth
   and again during the release audit. Reorged, pruned, or disappearing results
   fail the gate.
6. Machine-readable evidence and a browser screenshot agree on network, hash,
   type, block ID/hash, finality, program/account references, UTC observation
   time, source commit, and explorer URLs. Sensitive witness, identity, seed,
   key, message, and attachment material is excluded.
7. A second verifier can repeat the lookup from a clean network/browser context
   without repository-local state or credentials.

The evidence state machine is:

`built-local -> submitted -> sequencer-included -> indexer-seen ->`
`explorer-validated -> finalized`

Only `explorer-validated` or `finalized` can satisfy a public testnet transaction
requirement. A later failed recheck moves the artifact back to `disputed` until
the chain/indexer mismatch is explained and a canonical transaction is proven.

### Current Evidence Correction

As observed on 2026-08-12, the official explorer renders `Transaction not found`
for transaction
`d033cfe9a59a97824711f2a4d3df571281adc739e196cba1a7cf2264958298ad`
and `Block not found` for block `4035`, even though the sequencer RPC previously
returned that association. Therefore:

- `evidence/testnet/settlement-program.json` is only
  `sequencer-included-unreconciled` evidence;
- it must not satisfy `BOND-05`, deployment, explorer, demo, release, or
  submission gates in its current form;
- Chunk 20A must determine whether this is indexer lag, a divergent/noncanonical
  sequencer view, reset/pruning, wrong network assumptions, or invalid
  transaction construction before any replacement claim is made; and
- the existing transaction must not be resubmitted blindly. First reconcile the
  explorer/indexer tip and canonical block history, then use the official wallet
  path if a replacement transaction is required.

**Chunk 20A resolution, observed 2026-08-12T08:27:21Z:** the official explorer
subsequently resolved the exact transaction and finalized block 4035. Three
overlapping finalized blocks (4092-4094) matched the sequencer by exact hash, and
the newest finalized height was 4094 on both services. The original artifact is
preserved under `evidence/testnet/history/`; the current evidence was generated
by committed verifier `e4a33e36cfce8b9e8547ee4aa8612d0b948dd20b`. Because the
earlier sequencer finality and private indexer state were not captured, its
historical failure is classified only as resolved `pending-indexer-or-finality`,
not as proven indexer lag or divergence. Chunk 20B remains required before any
new release-path submission or program call.

## Delivery Waves

| Wave | Chunks | Outcome |
|---|---|---|
| 0 | 00 | Verified upstream contract and executable architecture |
| 1 | 01-04 | Buildable module skeleton, domain model, secure state, runtime |
| 2 | 05-09 | Messaging, storage, wallet, bond program, settlement foundations |
| 3 | 10-14 | Complete inbox product, all default skills, A2A coordination |
| 4 | 15-17 | Deployment CLI, Basecamp integration, operational resilience |
| 5 | 18-19 | Full local/standalone verification and security review |
| 5R | 20A-20F | Corrective explorer-validated LEZ testnet transaction track |
| 5Q | 20 | Performance, CU, and final testnet qualification |
| 6 | 21-22 | Reproducible demos, documentation, and submission |

Chunks in the same wave may run in parallel only after their shared dependencies
and interface contracts are complete.

## Execution Status (2026-08-14)

The authoritative requirement-level status remains
`docs/requirements/traceability.md`. This summary records the current execution
boundary without weakening any chunk exit criterion:

- Chunks 00-19 and 21-22 have their local implementation/checkpoint artifacts;
  their live upstream, testnet, performance, video, release, and submission gates
  remain open wherever the traceability matrix says `implemented` or `planned`.
- Chunks 06-07 now bind the exact pinned Logos Delivery and Storage generated
  clients in the production Core context. Adapter tests and the pinned module
  build qualify the local boundary; live multi-process interoperability and
  fault injection remain required.
- Chunk 20A and the canonical settlement publication part of 20C are reconciled.
  Chunk 20B standalone wallet qualification is 4/5 while the real private-proof
  case remains open with `RISC0_DEV_MODE=0`. A single-thread attempt exhausted
  its six-hour budget without a protocol rejection; a later twelve-hour attempt
  was externally interrupted after about two hours and produced no evidence.
  RISC Zero 3.0.5 does not checkpoint this local proof, so the next fresh run
  must use an operator-owned long-running service and bind its explicit local
  prover/thread settings into one complete five-case artifact.
- Chunk 20E wallet provisioning evidence passes for sender, owner, sink, and
  sender funding. Sink
  transaction
  `2f1df54d2658dc6ee45ae3eaac43902fcf548462cba7ff6599ca0670abc759ac`
  is finalized in block 4891 and has two public-explorer observations separated
  by six finalized blocks plus paired fresh-browser captures. Funding transaction
  `05fb87d3eda87fad090eafd1b5cff7b001faf8df594eb81ba74ee00e5e4f9b15`
  is finalized in block 5661, reconciles an exact 150-unit sender credit, and has
  paired explorer captures and separated machine observations.
- The eight Chunk 20D lifecycle rows and three Chunk 20E spending/paid-task rows
  remain open (11 of 16 inventory rows). CU measurements, three live profile
  deployments, the independent release audit, narrated video, tag, and
  submission also remain open. No submission pull request may be created
  without explicit owner instruction.
- Both public mutation matrices now force and journal one explicit local RISC
  Zero backend/thread profile. The three signed value operations additionally
  have a release-bound sequential runner that verifies every authorization,
  signer map, account, amount, and finalized candidate before advancing. These
  controls are locally verified; they do not count as any of the 11 missing
  explorer rows.

---

## Chunk 00 - Upstream Reconnaissance and Acceptance Freeze

**Depends on:** none

**Objective:** turn the brief and current upstream projects into a versioned,
testable implementation contract before code is scaffolded.

**Work:**

- Inspect LP-0008, Lambda Prize terms/evaluation policy, solution template, and
  current open/claimed/submitted status.
- Build a requirement traceability matrix covering every success criterion,
  default skill signature, Bonded Inbox MVP item, security boundary, demo flow,
  and submission artifact.
- Validate the current `logos-module-builder`, Logos Core/SDK, Basecamp, Storage,
  Delivery/Messaging, Execution Zone, LEZ programs, SPEL, Risc0, and A2A APIs.
- Run minimal upstream examples in a disposable spike workspace to prove module
  loading, Qt Remote Objects calls, messaging, storage, shielded wallet access,
  standalone sequencer operation, and Basecamp module loading.
- Decide whether bond escrow can safely extend an existing LEZ primitive or
  needs a new program; document privacy, refund, and expiry implications.
- Select and pin compatible commits/tags in an upstream compatibility matrix.
- Write ADRs for runtime structure, persistence, crypto/key ownership, A2A
  transport binding, settlement program, classifier boundary, and UI packaging.
- Record exact toolchain prerequisites and resource expectations for real proof
  generation in local development and CI.

**Deliverables:** `docs/requirements/traceability.md`,
`docs/architecture/upstream-compatibility.md`, ADRs, and a risk register.

**Exit criteria:** every requirement has an owner and verification route; all
critical upstream calls have a proven or documented API; no unresolved unknown
can force a fundamental redesign after Wave 1.

---

## Chunk 01 - Repository, Toolchain, and CI Skeleton

**Depends on:** 00

**Objective:** create the clean, licensed, reproducible foundation all later
chunks use.

**Work:**

- Add the selected license, contribution/security policies, code ownership,
  formatting, linting, secret scanning, and dependency update policy.
- Scaffold the pinned Nix flake, `metadata.json`, CMake targets, generated
  universal module interface, test targets, QML module, CLI, and documentation.
- Establish directories for runtime, domain, skills, integrations, profiles,
  UI, CLI, tests, LEZ program, scripts, evidence, and docs.
- Configure debug/release/sanitizer builds and deterministic dependency locks.
- Add CI stages for formatting, static analysis, unit tests, integration tests,
  package builds, docs/link checks, secret scanning, and the gated real-proof
  end-to-end suite.
- Add fixture-only development profiles with clearly non-production keys.

**Deliverables:** buildable empty module/package, test harness, CI workflows,
developer shell, and `CONTRIBUTING.md`.

**Exit criteria:** a clean clone can enter the dev shell, build all skeleton
targets, run the placeholder test suite, and reproduce the same dependency graph
locally and in CI.

---

## Chunk 02 - Domain Model and State Machines

**Depends on:** 00

**Objective:** specify the invariants that prevent lost messages and stranded or
incorrectly moved bonds.

**Work:**

- Define typed identifiers, money units, timestamps, addresses, content
  references, policy versions, signatures, and canonical serialization.
- Model signed/versioned inbox policies, trusted contacts, rate limits,
  attachments, owner approvals, private receipts, and profile capabilities.
- Define the message lifecycle: created, bond-pending, bonded, delivery-pending,
  pending-review, accepted, rejected, expired, delivery-failed, and settled.
- Define the A2A task lifecycle and its mapping to Logos Messaging events and
  LEZ payment/refund states.
- Specify legal transitions, terminal states, timeout semantics, idempotency
  keys, duplicate handling, optimistic concurrency, and error taxonomy.
- Create sequence diagrams for legitimate acceptance, spam rejection, expiry,
  failed delivery, attachment processing, above-limit approval, cancellation,
  and restart recovery.

**Deliverables:** domain types, pure transition reducers, schemas, diagrams, and
an invariants document.

**Exit criteria:** table-driven and property-based tests reject illegal
transitions and prove that every terminal path has exactly one valid bond
outcome.

---

## Chunk 03 - Secure Persistence and Event Processing

**Depends on:** 01, 02

**Objective:** provide crash-safe local state without leaking sensitive data.

**Work:**

- Implement the chosen transactional embedded store, schema versioning, atomic
  migrations, integrity checks, backup/restore, and corruption diagnostics.
- Persist policy snapshots, message/task state, bond references, approvals,
  storage references, receipts, retry schedules, and processed event IDs.
- Implement transactional inbox/outbox processing, compare-and-set transitions,
  replay windows, and bounded retry/dead-letter handling.
- Encrypt sensitive records at rest with keys supplied by the runtime key
  provider; never persist plaintext attachments or recoverable secrets.
- Add structured, redacted audit events with stable correlation IDs.

**Deliverables:** persistence library, migration tooling, recovery tests, and
redaction tests.

**Exit criteria:** kill/restart and duplicate-event tests preserve all pending
work, never double-settle, and reveal no forbidden material in logs or database
inspection fixtures.

---

## Chunk 04 - Core Module Runtime, Identity, Profiles, and Skill SDK

**Depends on:** 01, 02, 03

**Objective:** implement the loadable agent runtime and extensibility boundary.

**Work:**

- Implement the generated Logos Core module entry point and Qt Remote Objects
  API without modifying upstream modules.
- Add lifecycle management, dependency readiness checks, graceful shutdown,
  health/status, configuration validation, and profile loading.
- Integrate per-agent NPK/ISK shielded identity and derive or bind the Logos
  Messaging identity according to the verified upstream API.
- Define a versioned skill manifest, typed input/output schema, permissions,
  cancellation, deadlines, progress events, and structured errors.
- Add discovery/registration, concurrent execution, per-skill resource limits,
  and process/thread isolation sufficient to stop one failing skill from
  crashing or blocking others.
- Enforce profile capability allowlists and expose an SDK/template plus a sample
  third-party skill that requires no core-module edits.
- Support a pluggable inference interface with a deterministic no-model fallback.

**Deliverables:** loadable runtime, skill SDK, sample skill, three profile
manifests, and module/SDK tests.

**Exit criteria:** Logos Core loads/unloads the module; each profile exposes only
its allowed capabilities; a crashing/timing-out sample skill is isolated; an
external sample skill builds and loads without changing runtime code.

---

## Chunk 05 - Cryptographic Envelopes, Policies, and Key Boundaries

**Depends on:** 02, 04

**Objective:** make messages, policies, receipts, and delegated access
authentic, confidential, versioned, and replay-resistant.

**Work:**

- Define canonical signed envelopes for inbox policies, message metadata, A2A
  cards/tasks, approvals, settlement instructions, and receipts.
- Use audited upstream cryptographic primitives for encryption, signatures, key
  derivation, and secure randomness; do not invent cryptography.
- Bind signatures to protocol version, network, agent identity, policy version,
  message/task ID, nonce, and expiry where appropriate.
- Implement key-provider interfaces, least-privilege key access, rotation, secure
  deletion behavior, and test-only deterministic providers.
- Add policy immutability after message submission and verification of stale,
  malformed, cross-network, and downgraded envelopes.

**Deliverables:** envelope library, key boundary documentation, golden vectors,
and adversarial verification tests.

**Exit criteria:** tampering, replay, wrong-recipient, expired, stale-policy, and
cross-network tests fail closed; logs and error strings contain no secrets or
plaintext.

---

## Chunk 06 - Logos Messaging and Owner Channel Integration

**Depends on:** 04, 05

**Objective:** provide real-time encrypted owner and agent communication without
an intermediary server.

**Work:**

- Wrap the pinned Delivery/Messaging module APIs behind an injectable adapter.
- Implement startup, topic subscription, send/receive, store/history recovery,
  reconnect, backpressure, acknowledgement, deduplication, and delivery status.
- Establish the dedicated end-to-end encrypted owner topic and authenticate all
  commands, approvals, configuration changes, and notifications.
- Define content topics and envelopes for inbox intake, owner review, A2A
  discovery/tasks, and private receipts without exposing message content in
  topic names or telemetry.
- Implement `messaging.send`, `messaging.join`, and
  `messaging.create_group` with permission and input validation.

**Deliverables:** Messaging adapter, owner-channel service, three Messaging
skills, integration harness, and protocol documentation.

**Exit criteria:** two separate Logos instances exchange real-time encrypted
messages; offline/reconnect delivery is deduplicated; unauthorized owner
commands fail; all three skills pass contract and integration tests.

---

## Chunk 07 - Logos Storage and Vault Agent

**Depends on:** 04, 05

**Objective:** store and selectively share encrypted files while keeping keys
and plaintext local to authorized agents.

**Work:**

- Wrap the pinned Storage module API behind an injectable adapter.
- Implement streaming encryption/decryption, integrity verification, upload,
  download, local metadata indexing, deletion policy, and safe temporary files.
- Implement `storage.upload`, `storage.download`, `storage.list`, and
  `storage.share` with labels, content addresses, and recipient grants.
- Enforce the MVP limit of one attachment per message plus configured type and
  size rules before storage or processing.
- Add scoped, expiring access grants for the owner or approved processing agent,
  with revocation and audit events.
- Implement the Vault profile and personal-file-vault owner flow.

**Deliverables:** Storage adapter, four Storage skills, Vault Agent profile,
encrypted attachment service, and integration tests.

**Exit criteria:** storage operators and unauthorized agents cannot read test
plaintext; corrupted content and invalid grants fail closed; upload/download/
list/share and expiry/revocation work against the actual Storage module.

---

## Chunk 08 - Shielded Wallet and Spending Controls

**Depends on:** 03, 04, 05, and the wallet API decision from 00

**Objective:** give each agent independent shielded funds with enforceable owner
limits.

**Work:**

- Integrate the pinned official LEZ wallet/account transaction builder, proof
  path, and confirmation/finality model. A direct raw sequencer submission may
  exist only as a diagnostic tool and cannot produce release evidence.
- Persist separate submitted, sequencer-included, indexer-seen,
  explorer-validated, finalized, disputed, and failed transaction states.
- Implement `wallet.balance`, `wallet.send`, and `wallet.history` without
  exposing private wallet history beyond the authorized owner response.
- Implement `program.query`, `program.call`, and `program.deploy` with canonical
  parameter encoding and network/program allowlists.
- Enforce per-transaction and rolling-period limits atomically across concurrent
  operations and restarts.
- Persist above-limit proposals, notify/retry the owner, verify signed approval
  or denial, expire unanswered requests without execution, and prevent TOCTOU
  changes between proposal and submission.
- Separate operational spending from bond escrow/refunds so owner limits cannot
  strand sender funds.

**Deliverables:** Wallet/Program adapters, six Blockchain skills, approval
service, and ledger reconciliation tests.

**Exit criteria:** below-limit operations execute autonomously; above-limit
operations never execute before valid approval; timeout, denial, concurrent
spend, retry, and restart cases preserve limits; each profile has an independent
shielded test account; no adapter reports final success before the configured
finality state, and public testnet success additionally requires Chunk 20F.

---

## Chunk 09 - LEZ Bond Program and Client

**Depends on:** 02, 05, 08, and the settlement ADR from 00

**Objective:** implement non-custodial, replay-protected bond lifecycle logic.

**Work:**

- Define public/private program state so message content, sender identity, inbox
  policy details beyond necessary commitments, and wallet history remain private.
- Implement bond lock with committed message ID, policy version/hash, amount,
  deadline, fixed sink/community pool, and authorized settlement conditions.
- Implement full refund on acceptance, expiry, and failed delivery; fixed-sink
  transfer on explicit spam rejection or deterministic violation; prohibit any
  transfer to the inbox owner.
- Make settlement terminal, idempotent, amount-safe, replay-protected, and robust
  to duplicate proofs/transactions and late/conflicting decisions.
- Implement client-side proof/transaction creation, confirmation tracking,
  reconciliation, and private receipt material through the official wallet path.
  Track the submitted hash through sequencer, indexer, explorer, and finality;
  never treat a locally computed hash or RPC response as explorer validation.
- Measure initial CU cost for every instruction and add standalone-sequencer
  program tests with `RISC0_DEV_MODE=0`.

**Deliverables:** auditable LEZ program, generated interface/client, program test
suite, threat notes, and initial CU report.

**Exit criteria:** conservation and destination invariants hold under property/
fuzz tests; owner-profit attempts and double settlement are impossible; every
bond terminal path succeeds on a real standalone sequencer with real proofs;
public testnet qualification remains pending until the full Chunk 20D matrix is
finalized and explorer-validated.

---

## Chunk 10 - Settlement Agent and Private Receipts

**Depends on:** 03, 06, 08, 09

**Objective:** reliably connect inbox decisions to confirmed on-chain outcomes.

**Work:**

- Implement bond confirmation before delivery to the review queue.
- Consume authenticated accepted/rejected/expired/delivery-failed instructions
  and submit exactly one legal settlement action.
- Add deadline scheduling, failed-delivery compensation, sequencer/indexer/
  explorer/finality polling with explicit intermediate states,
  stuck-transaction recovery, and chain/local reconciliation after restart.
- Generate signed private receipts with outcome, amount, program/transaction
  reference, policy commitment, timestamps, and verification instructions.
- Deliver receipts privately to sender and owner through Messaging.
- Implement the Settlement/Blockchain profile with least-privilege permissions.

**Deliverables:** Settlement Agent profile, reconciler, scheduler, receipt
schema/verifier, and fault-injection tests.

**Exit criteria:** all four settlement paths complete exactly once; process
termination at every persistence/network boundary recovers safely; failed
processing cannot strand a valid sender bond; a transaction missing from the
official explorer remains pending/disputed and cannot generate a successful
receipt or release evidence.

---

## Chunk 11 - Inbox Policy, Discovery, and Sender Intake

**Depends on:** 03, 05, 06, 09, 10

**Objective:** publish trustworthy inbox terms and accept bonded first contact.

**Work:**

- Implement policy creation, signing, validation, version history, publication,
  future-only updates, and QR/shareable inbox entry point.
- Publish bond amount, expiry, accepted content/attachment constraints, response
  deadline, settlement policy, and bond-free emergency channel in the signed
  Agent Card/policy document.
- Implement sender preflight, bond lock/confirmation, encrypted message submit,
  attachment reference, duplicate protection, and status tracking.
- Implement trusted-contact bypass with authenticated contact identity and
  revocation semantics.
- Enforce per-sender rate limits and deterministic format/type/size rules with
  privacy-preserving error responses.
- Implement the Inbox/Messaging profile and private owner review queue service.

**Deliverables:** Inbox Agent profile, sender client flow, policy service, QR/
address encoding, and end-to-end intake tests.

**Exit criteria:** senders see and sign against exact immutable terms; unconfirmed
or underfunded bonds never reach review; trusted contacts bypass correctly;
policy changes do not alter in-flight messages; duplicate submission is safe.

---

## Chunk 12 - Triage, Review, and Decision Safety

**Depends on:** 10, 11

**Objective:** classify messages locally while keeping financially consequential
decisions under deterministic or explicit owner control.

**Work:**

- Implement pluggable local classifier input/output contracts with calibrated
  score, reason codes, model/version metadata, timeout, and no-model fallback.
- Automatically accept only explicit configured cases such as trusted contacts
  or clear policy matches; quarantine uncertain content for owner review.
- Allow automatic rejection only for enumerated deterministic violations with
  evidence attached to the decision.
- Require authenticated explicit owner action for spam rejection based on model
  output and make the sink destination visible before confirmation.
- Add bulk review, rate-limit visualization data, decision audit events, and
  safeguards against prompt/content injection reaching command paths.

**Deliverables:** triage service, deterministic rule engine, review command API,
classifier plugin example, and adversarial tests.

**Exit criteria:** arbitrary model output cannot move funds or invoke skills;
uncertain/error cases quarantine safely; every rejection records either signed
owner intent or reproducible deterministic evidence.

---

## Chunk 13 - A2A Binding, Agent Cards, and Paid Coordination

**Depends on:** 05, 06, 08, 10

**Objective:** implement documented A2A-compatible discovery and task execution
over Logos Messaging with LEZ payment.

**Work:**

- Pin the A2A schema/version and document deviations/extensions for Logos
  Messaging transport, identity/signatures, encryption, and LEZ payment.
- Implement signed Agent Cards with identity, skills, JSON schemas, capabilities,
  endpoints/topics, and optional task price; publish to scoped discovery topics.
- Implement `agent.card` and `agent.discover` with signature/schema validation,
  expiry, deduplication, and trust policy.
- Implement `agent.task`, `agent.subscribe`, and `agent.cancel` with A2A working,
  input-required, completed, failed, and canceled/refund behavior.
- Couple task acceptance, escrow/payment, completion, cancellation, and refund
  idempotently; handle status/payment races and unavailable peers.
- Provide interoperability fixtures against the official A2A schema and, where
  practical, an independent A2A implementation at the protocol boundary.

**Deliverables:** five A2A skills, transport-binding specification, schemas,
paid-task coordinator, and interoperability tests.

**Exit criteria:** at least two agents discover one another, execute and stream a
task through the A2A lifecycle, and transfer/refund LEZ autonomously without
owner intervention; invalid cards/tasks/payments fail safely.

---

## Chunk 14 - Meta Skills and Complete Skill Conformance

**Depends on:** 06, 07, 08, 13

**Objective:** close the LP-0008 default-skill surface and prove it is consistent.

**Work:**

- Implement `meta.skills`, `meta.status`, and `meta.configure` with access
  control, validation, atomic updates, and secret-safe output.
- Ensure status reports balance, storage usage, active tasks, runtime health,
  profile, and dependency health without leaking private data.
- Generate skill documentation and JSON schemas from the same source used by
  runtime registration to prevent drift.
- Build a conformance suite for exact names, parameters, outputs, error shapes,
  cancellation, permissions, and profile availability of every required skill.
- Confirm third-party skill authoring, packaging, loading, version compatibility,
  and failure isolation using the sample external skill.

**Deliverables:** three Meta skills, generated skill reference, conformance
suite, and third-party skill tutorial.

**Exit criteria:** the traceability matrix shows all 21 required default skill
operations implemented and documented; conformance tests pass for the runtime
and relevant profiles.

---

## Chunk 15 - One-Command Headless Deployment and Operations CLI

**Depends on:** 04, 07, 08, 11, 13, 14

**Objective:** let an owner deploy, configure, fund, inspect, back up, and recover
an agent on any supported machine with no server or exposed API.

**Work:**

- Build a single deployment command that installs/loads Logos Core headless,
  selected modules, a profile, generated identity, owner channel, and persistent
  service configuration.
- Add non-interactive flags plus safe interactive prompts for network, profile,
  owner address, policy, spending limits, data directory, and initial funding.
- Add commands for plan/dry-run, status/health, logs, policy update, fund,
  approve/deny, backup/restore, upgrade, rollback, and clean test teardown.
- Make deployment repeatable and idempotent with explicit preflight checks,
  machine-readable output, and actionable failure messages.
- Package portable `.lgx` artifacts and pin/verify downloaded inputs.

**Deliverables:** deployment/operations CLI, packages, service templates, and
clean-machine deployment tests.

**Exit criteria:** a documented single command reproducibly deploys each profile
on a clean supported machine; a separate Logos app reaches the owner channel;
upgrade/rollback and backup/restore preserve pending state.

---

## Chunk 16 - Basecamp Owner Experience

**Depends on:** 06, 11, 12, 14

**Objective:** expose the complete owner workflow inside the Logos app while
respecting the established Basecamp module architecture.

**Work:**

- Implement loadable Basecamp/QML assets and backend replica wiring.
- Build inbox views for pending/accepted/rejected/expired/failed states, safe
  message/attachment viewing, search/filtering, and status updates.
- Build policy/trusted-contact/rate-limit/classifier/spending configuration with
  signed preview and validation before publication.
- Build owner review, explicit rejection confirmation, above-limit approval,
  task progress, storage grants, and private receipt verification flows.
- Cover loading, empty, offline, reconnecting, stale, error, and recovery states;
  meet keyboard, focus, contrast, scaling, localization, and screen-reader needs.
- Ensure sensitive values are not copied, cached, notified, or logged by default.

**Deliverables:** Basecamp UI module, interaction tests, accessibility report,
and owner workflow documentation.

**Exit criteria:** every routine owner action is available from Basecamp through
the encrypted owner channel; UI state remains consistent across reconnect and
restart; automated tests and desktop/mobile viewport checks show no overlap or
unreadable content.

---

## Chunk 17 - Reliability, Observability, and Recovery Hardening

**Depends on:** 03, 06-16

**Objective:** make the distributed workflow diagnosable and recoverable under
real failures without leaking private content.

**Work:**

- Define service-level indicators for intake, task, storage, approval, and
  settlement latency/failure while avoiding user-content metrics.
- Add structured redacted logs, health/readiness, bounded metrics, audit export,
  and correlation across Messaging, Storage, and LEZ references.
- Add circuit breakers, retry budgets, jittered backoff, backpressure, queue
  limits, clock-skew handling, dependency degradation, and disk-full behavior.
- Implement startup reconciliation for Messaging history, local state, Storage
  references, LEZ bonds, approvals, A2A tasks, and receipts.
- Run fault injection for disconnects, duplicate/reordered events, sequencer
  reorg/finality behavior as supported, Storage outage, process kill, host
  restart, corrupted record, slow skill, and unavailable owner.

**Deliverables:** operations runbook, dashboards/queries or local equivalents,
fault-injection suite, and recovery evidence.

**Exit criteria:** pending messages/tasks/bonds survive tested failures; approval
requests retry then fail closed; skill failures stay isolated; operators can
diagnose failures using only redacted telemetry.

---

## Chunk 18 - Verification Matrix and CI Completion

**Depends on:** 09-17

**Objective:** prove every functional and non-functional requirement at the
lowest useful layer and in a realistic end-to-end environment.

**Work:**

- Complete unit, property, fuzz, serialization-vector, component-contract,
  integration, UI, CLI, migration, packaging, and end-to-end suites.
- Add explicit financial invariant, replay, authorization, privacy/redaction,
  concurrency, timeout, and crash-consistency coverage.
- Build a hermetic multi-agent environment with Logos Core, Messaging, Storage,
  three profiles, owner/sender clients, and standalone LEZ sequencer.
- Run the complete lifecycle with real proof generation and
  `RISC0_DEV_MODE=0`; assert the environment rather than trusting caller input.
- Split fast pull-request checks from protected release/evaluation checks without
  weakening the required default-branch green result.
- Map each traceability row to stable test IDs and retain sanitized artifacts on
  failure.
- Keep local/standalone verification separate from public testnet qualification.
  CI may prove code correctness locally, but only Chunk 20F may promote a public
  transaction requirement to `verified-testnet`.

**Deliverables:** complete automated suite, CI workflows, coverage/traceability
report, and evaluator-mode test command.

**Exit criteria:** all traceability rows have passing automated evidence or a
documented manual evidence step; a clean evaluator-style clone runs the stated
command without modification; default-branch CI is green; public LEZ rows remain
pending until the explorer evidence gate passes.

---

## Chunk 19 - Security, Privacy, and Economic Abuse Review

**Depends on:** 17, 18

**Objective:** challenge trust boundaries and economic assumptions before public
testnet deployment.

**Work:**

- Threat-model assets, actors, trust boundaries, key compromise, malicious
  senders/owners/agents, storage operators, messaging peers, and chain observers.
- Review authentication, authorization, parser limits, signature domains,
  attachment handling, temporary files, key lifecycle, dependency/supply chain,
  UI confirmations, and CLI secret handling.
- Test spam bursts, Sybil senders, bond griefing, owner rejection abuse, sink
  manipulation, expiry races, underpayment, fee/CU exhaustion, paid-task fraud,
  cancellation races, and low-balance exclusion.
- Fuzz all untrusted network, file, QR, CLI, policy, Agent Card, and A2A inputs.
- Run sanitizer/static/dependency/secret/license scans and remediate findings.
- Document residual risks, small-bond guidance, trusted-contact exemptions, and
  the required bond-free emergency channel.

**Deliverables:** threat model, privacy analysis, economic abuse analysis,
security test report, SBOM, and resolved finding log.

**Exit criteria:** no unresolved critical/high issue; all key boundaries and
financial invariants have adversarial tests; residual risks and operator
mitigations are explicit.

---

## Chunk 20A - Reconcile Sequencer, Indexer, and Explorer Canonical State

**Depends on:** 18, 19, and the LEZ Explorer Validation Contract

**Objective:** explain the current sequencer/explorer disagreement and establish
one canonical public testnet observation path before creating more transactions.

**Work:**

- Pin the official sequencer, explorer, indexer API if publicly exposed, LEZ
  release commit, explorer build/version, expected chain identity/genesis, and
  finality semantics in the upstream compatibility matrix. Do not infer that two
  services share a chain merely because their hostnames share a domain.
- Capture a time-correlated observation from the sequencer and explorer:
  health, latest block ID/hash/timestamp, indexer sync/stall status where
  available, and several overlapping finalized block IDs/hashes.
- Compare the sequencer block containing each candidate transaction with the
  explorer's canonical block at the same height. Classify each mismatch as
  indexer lag, stalled indexer, divergent chain, reset/pruning, reorg, wrong
  endpoint, invalid transaction encoding, or unresolved upstream incident.
- Recheck the current deployment transaction and block only after the explorer
  tip reaches or passes block 4035. Record whether the exact hash appears in the
  explorer transaction page and the exact block contains it.
- Define bounded indexer-lag and confirmation timeouts from observed testnet
  behavior. A timeout produces `pending-indexer` or `disputed`, never `verified`.
- Add an incident record for unresolved upstream inconsistency with raw sanitized
  request/response timestamps and enough information for Logos maintainers to
  reproduce it. Do not work around a divergent chain by choosing whichever
  endpoint makes the evidence pass.
- Reclassify existing evidence and traceability to its proven state. Preserve the
  old artifact for audit history; never edit an old hash/block into looking like
  a successful explorer validation.

**Deliverables:** canonical-network observation report, sequencer/indexer tip
comparison, current-transaction reconciliation report, incident template, and
an evidence status vocabulary shared by scripts and traceability.

**Exit criteria:** at least three overlapping finalized block ID/hash pairs agree
between the sequencer and explorer; indexer freshness can be measured; the
current deployment is either independently explorer-validated or explicitly
retained as disputed/unreconciled with a documented cause; no task relies on
block 4035 until the explorer itself resolves it.

---

## Chunk 20B - Official LEZ Wallet and Transaction Construction Path

**Depends on:** 08, 09, 20A

**Objective:** replace ad hoc transaction submission with the pinned official
LEZ wallet/client path used for canonical program deployment and shielded calls.

**Work:**

- Pin the exact official LEZ wallet CLI/library and all transaction/proof
  dependencies to the same release as the public testnet. Record the supported
  program-deployment, account initialization, funding, public invocation, and
  privacy-preserving invocation commands/APIs.
- Implement one adapter around the official wallet transaction builder. It must
  obtain hashes from the signed/serialized transaction it actually submits, not
  manufacture an expected hash from a parallel custom encoder.
- Keep the existing encoder only as a conformance oracle until byte-for-byte
  vectors match official wallet output; it cannot be the release submission path.
- Configure the official testnet endpoint, network domain, proof mode, finality,
  and indexer/explorer URLs from one validated network profile. Refuse mixed
  endpoints or unpinned network metadata.
- Provision disposable testnet-only identities through the official wallet/KMS
  path, store secrets outside the repository, redact commands/logs, and document
  funding/faucet prerequisites. Never print or persist seeds in evidence.
- Add dry-run/inspect output with transaction kind, program ID, account IDs that
  are intentionally public, payload digest, proof mode, expected fee/CU, and
  network identity before an operator authorizes submission.
- Add local standalone-sequencer tests for official-wallet deployment and real
  privacy-preserving invocation with `RISC0_DEV_MODE=0`, including malformed
  instruction, wrong network, insufficient funds, and duplicate submission.
- Fail closed if the pinned wallet cannot construct the transaction type needed
  by the current guest. Document the upstream gap instead of falling back to an
  unaudited raw RPC payload for release evidence.

**Deliverables:** pinned official wallet adapter/CLI wrapper, network profile,
secret-safe identity/funding runbook, official-vs-local serialization vectors,
and standalone real-proof integration tests.

**Exit criteria:** an official-wallet-built program deployment and a shielded
program call both succeed on the pinned standalone stack with real proofs; their
serialized digests and hashes are reproducible; logs contain no secret material;
the release path contains no direct handcrafted `sendTransaction` shortcut.

---

## Chunk 20C - Explorer-Validated Settlement Program Publication

**Depends on:** 20A, 20B

**Objective:** obtain a canonical settlement-program deployment transaction that
an evaluator can independently validate on the official LEZ testnet explorer.

**Work:**

- Rebuild the release guest from a clean clone with locked dependencies and
  `RISC0_DEV_MODE=0`; record source commit, binary digest/size, toolchain, and
  program/image ID before submission.
- Query the explorer/indexer for the program ID and candidate transaction first.
  Reuse an existing canonical deployment only if the explorer validates its exact
  binary digest/size and program ID; otherwise prepare one new deployment.
- Submit through the official wallet path from Chunk 20B. Capture sanitized
  official-wallet output, submitted bytes digest, returned hash, and UTC time.
- Poll both sequencer and indexer independently. Require matching transaction
  type `Program Deployment Transaction`, exact hash, containing block ID/hash,
  bytecode size, program ID derivation, and finality.
- Open the exact explorer transaction and block URLs in a fresh browser context.
  Save full-page screenshots after loading completes and assert expected text;
  an HTTP 200 page containing `Transaction not found` is a failure.
- Recheck after confirmation depth and after at least one independent later
  observation. If the explorer loses the transaction, mark evidence disputed and
  stop downstream testnet work.
- Store a new immutable evidence artifact. Keep the old `d033...` artifact clearly
  labeled unreconciled; never overwrite it with the new identifiers.

**Deliverables:** canonical program deployment JSON, raw sanitized sequencer and
indexer observations, transaction/block explorer screenshots, SHA-256 manifest,
and public explorer links.

**Exit criteria:** a clean browser resolves the exact deployment hash and block;
the explorer shows `Program Deployment Transaction` and the correct bytecode
size; the block contains the hash and is finalized; an independent verifier
repeats the checks; traceability may then mark program publication
`verified-testnet`.

---

## Chunk 20D - Explorer-Validated Bond Lifecycle Matrix

**Depends on:** 09, 10, 20C

**Objective:** prove the deployed program actually executes every financial path
on public LEZ testnet, rather than proving only that bytecode was uploaded.

**Required transaction matrix:**

| Case | Required transactions | Required state/balance assertion |
|---|---|---|
| Acceptance | funded sender/account initialization as needed; bond `Initialize`; owner-authorized `Settle(RefundAccepted)` | exact bond locked once, then full amount returned to sender; owner receives zero |
| Rejection | fresh bond `Initialize`; owner-authorized `Settle(SinkRejected)` | full amount reaches the fixed sink; sender is not refunded; owner receives zero |
| Expiry | fresh bond `Initialize`; post-deadline `Settle(RefundExpired)` | early attempt fails without state change; valid expiry returns full amount to sender |
| Delivery failure | fresh bond `Initialize`; owner-authorized `Settle(RefundDeliveryFailed)` | full amount returns to sender; owner and sink receive zero |
| Replay/conflict | replay each terminal call and submit one conflicting outcome | no second successful state transition or value movement; failed attempts are recorded separately and never presented as successful explorer transactions |

**Work:**

- Use fresh unique bond/message/policy commitments for each case and record a
  privacy-safe case ID to transaction mapping.
- Build and submit every successful call through the official shielded wallet
  path with real proofs and `RISC0_DEV_MODE=0`. Record actual CU/fee data.
- Capture pre-lock, post-lock, and post-settlement account/state observations.
  Prove conservation from authoritative wallet/indexer state without disclosing
  private keys, message content, attachments, or unnecessary identity history.
- Require each successful `Initialize` and `Settle` hash to pass the complete LEZ
  Explorer Validation Contract. Link each settlement to its corresponding lock,
  program ID, outcome, final block, and private receipt.
- Exercise early expiry, unauthorized settlement, wrong destination, wrong PDA,
  duplicate, and conflicting-outcome attempts. Prove rejection locally and from
  wallet/sequencer responses; do not require a nonexistent explorer transaction
  for a request rejected before canonical inclusion.
- Re-run all exact explorer URLs after confirmation depth and during the release
  audit. Any missing successful transaction invalidates that matrix row.

**Deliverables:** per-case immutable evidence bundles, transaction map, redacted
proof/CU logs, explorer screenshots and URLs, account/state reconciliation, and
a machine-readable lifecycle summary.

**Exit criteria:** every successful transaction in all four positive paths is
finalized and independently explorer-validated; balances/state prove the required
destination and amount; owner profit is zero; replay/conflict attempts cause no
second movement; no case relies only on a deployment transaction or local model.

---

## Chunk 20E - Explorer-Validated Wallet, Profile, and Use-Case Transactions

**Depends on:** 13-17, 20D

**Objective:** attach independently viewable public LEZ transactions to the
Blockchain profile, paid coordination, and every submission claim that moves
testnet value.

**Work:**

- Deploy Inbox, Vault, and Settlement as separate runtime profiles with separate
  shielded identities, configuration, data directories, and least-privilege
  capabilities. Do not equate program publication with an agent deployment.
- For the Settlement profile, execute and explorer-validate at minimum one
  official-wallet funding/transfer, one `program.deploy` or canonical reference,
  one `program.call`, one `program.query` state check, and one wallet-history
  reconciliation. Queries without transactions need signed/machine-readable
  state evidence, not fabricated transaction hashes.
- Execute one below-limit autonomous transfer and one above-limit transfer after
  signed owner approval. Prove the unanswered/denied case submits no transaction
  by correlating proposal IDs, wallet history, account state, and elapsed expiry.
- Execute a paid A2A task between independent agents and validate its payment or
  escrow/refund transactions on the explorer. Link the A2A task state to exact
  transaction hashes without placing task content on-chain.
- Execute the on-chain event alerter against a real finalized Bonded transaction
  and show one deduplicated private owner notification containing the public hash.
- Ensure every evidence bundle identifies the responsible profile, release
  commit, program ID, transaction URLs, block URLs, finality, and independent
  verifier result while excluding seeds and private histories.

**Deliverables:** three profile evidence bundles, spending-control transaction
bundle, paid-task transaction bundle, event-alerter correlation evidence, and a
public explorer link index.

**Exit criteria:** each profile is independently running and reproducible; every
successful value-moving claim has a finalized explorer transaction; every
non-execution claim proves absence without inventing a hash; paid-task and owner
approval flows reconcile application state, wallet state, and explorer state.

---

## Chunk 20F - Automated Explorer Evidence Gate and Independent Audit

**Depends on:** 20A-20E

**Objective:** make unverifiable, stale, mismatched, or screenshot-only evidence
fail CI/release qualification automatically.

**Work:**

- Define a versioned JSON schema requiring evidence status, network identity,
  sequencer/indexer/explorer endpoints, source commit, operation/case/profile,
  transaction hash/type, block ID/hash/finality, program/account references,
  explorer URLs, UTC observations, confirmation depth, artifact hashes, and
  independent verifier metadata.
- Build a read-only verifier that fetches each exact transaction and block from
  official services, parses explorer-rendered state, rejects error/loading/not-
  found pages, cross-checks all identifiers, and emits deterministic results.
- Add fixtures for valid transactions, transaction-not-found with HTTP 200,
  block-not-found, indexer lag, divergent block hash, wrong transaction type,
  wrong program ID, pending finality, reorg/disappearance, malformed evidence,
  duplicate hash reused for different operations, and leaked secret fields.
- Require two observations separated by confirmation depth and a clean-browser
  screenshot. Screenshots are supporting artifacts only; machine validation is
  authoritative and both must agree.
- Add a release/evaluator command that verifies every required matrix row and
  profile bundle without submitting anything. Network outages yield
  `verification-unavailable`, never success.
- Run an independent clean-clone audit from a separate browser/network context.
  Record verifier version, time, results, and hashes of reviewed artifacts.
- Make traceability generation reject `verified-testnet` unless a passing,
  current explorer evidence bundle exists. Recheck immediately before video,
  release tag, and submission authorization.

**Deliverables:** evidence schema, read-only explorer verifier, adversarial
fixtures/tests, CI/release gate, evidence index, and independent audit report.

**Exit criteria:** the current `d033...` evidence fails until the explorer resolves
it; every required new positive transaction passes exact transaction and block
lookups twice; tampered/stale/mismatched evidence fails; a clean-clone audit
reproduces the complete public transaction matrix without access to secrets.

---

## Chunk 20 - Performance, CU, and Testnet Qualification

**Depends on:** 18, 19, 20A-20F

**Objective:** establish operational limits and produce reproducible LEZ testnet
deployment evidence.

**Work:**

- Benchmark startup, memory, queue throughput, spam burst behavior, attachment
  streaming, A2A concurrency, reconnect/recovery, and settlement latency.
- Measure and document CU cost for token transfers and every program query,
  call, deployment, bond instruction, and paid-task operation on the selected
  devnet/testnet version.
- Define supported limits and tune queue, rate, size, timeout, and concurrency
  defaults without compromising correctness.
- Deploy Inbox/Messaging, Vault/Storage, and Settlement/Blockchain agents with
  separate identities and reproducible commands on LEZ testnet.
- Capture sanitized network/version, Agent Card, program ID, transaction/proof,
  deployment log, timestamp, exact transaction/block explorer URLs, finality,
  and independent verification evidence for each profile.
- Run Chunk 20F's read-only gate over the deployment, complete bond lifecycle,
  spending-control, paid-task, and profile bundles before signing qualification.
- Repeat all release tests against the exact pinned release candidate.

**Deliverables:** benchmark report, CU table with network/version/date,
performance limits, three testnet deployment evidence bundles, and signed release
candidate manifest.

**Exit criteria:** documented limits hold under target load; CU data covers every
on-chain operation; all three independently identifiable testnet agents are live,
discoverable, reproducible, and linked to sanitized evidence; every successful
value-moving transaction is finalized and resolves on the official explorer by
exact hash and block; the read-only evidence gate and independent audit both
pass against the release candidate.

---

## Chunk 21 - Reproducible Use Cases and Narrated Demo

**Depends on:** 20

**Objective:** provide evaluator-runnable proof of the product and at least three
named LP-0008 illustrative use cases.

**Work:**

- Create one evaluator script that provisions the real local stack, asserts
  `RISC0_DEV_MODE=0`, executes flows, validates outcomes, and cleans only its own
  disposable resources.
- Demonstrate legitimate bonded first contact: lock, deliver, owner accept, full
  refund, and private receipt.
- Demonstrate spam burst: rate limiting, local triage/quarantine, explicit or
  deterministic rejection, fixed-sink settlement, and no on-chain plaintext.
- Demonstrate encrypted attachment storage plus a paid A2A processing task within
  the spending limit.
- Demonstrate an above-limit action waiting for signed owner approval and an
  unanswered request timing out without execution.
- Demonstrate restart recovery with pending messages, A2A tasks, approvals, and
  bonds preserved.
- Explicitly label three official illustrative use cases: **Personal file vault**
  (Vault), **Paid skill marketplace / privacy-preserving agent pipeline**
  (attachment processing), and **On-chain event alerter** (bond/program event
  owner notifications).
- Re-run the read-only explorer evidence gate immediately before recording and
  show the exact successful transaction and block pages for deployment, bond
  lock, settlement, and paid-task payment/refund in a fresh browser context.
- Record a narrated video showing architecture, key decisions, all required
  flows, explorer-validated testnet evidence, terminal output, and visible real
  proof generation.

**Deliverables:** idempotent demo script, fixtures, expected-output checks,
narration/run sheet, video, captions/transcript, and evidence index.

**Exit criteria:** a clean-machine rehearsal succeeds without edits; all money,
message, storage, approval, privacy, and recovery assertions pass; the recording
meets Lambda narration and proof-visibility requirements; every on-chain claim
shown in the recording has a still-valid exact explorer URL in the evidence
index.

---

## Chunk 22 - Documentation, Release Audit, and Submission

**Depends on:** 21

**Objective:** make the repository independently understandable, reproducible,
and ready for first-come-first-served evaluation.

**Work:**

- Complete the README with prerequisites, architecture, quick start, single-
  command deployment, configuration, funding, owner interaction via CLI/Basecamp,
  demo, tests, troubleshooting, and supported versions.
- Publish the skill interface/SDK guide, generated default-skill reference, A2A
  Messaging transport binding, bond protocol, spending controls, security model,
  privacy model, deployment/operations guide, owner guide, and known limitations.
- Verify license headers, dependency licenses, SBOM, clean history, absence of
  secrets/private artifacts, links, screenshots, and copy-paste commands.
- Re-run the evaluator path from a fresh clone and archive CI, CU, testnet,
  deployment, and demo evidence against the release commit.
- Re-run Chunk 20F from a clean network/browser context immediately before the
  release tag and again before submission authorization. Any missing, disputed,
  non-final, or mismatched explorer transaction blocks the release.
- Fill every requirement traceability row and conduct an independent release
  checklist review; remove `verified-testnet` from any row whose current explorer
  artifact does not pass.
- Prepare the Lambda Prize solution document/PR using the official template and
  include repository, release, video, three deployment, and evidence links.
- Re-check prize status, terms, and submission limits immediately before the
  authorized public submission; do not submit or publish credentials
  automatically.

**Deliverables:** final documentation set, tagged release, release checksums,
completed traceability matrix, evidence archive, and submission draft.

**Exit criteria:** an independent reviewer can clone, build, deploy, operate, and
run the demo solely from repository docs; every LP-0008 criterion points to
specific passing evidence; every on-chain claim resolves without credentials on
the official explorer and its finalized block; the submission draft is ready for
explicit owner approval.

---

## Final Requirement Gate

Do not call the project complete until this table is entirely checked and linked
to evidence in the traceability matrix.

- [ ] Official sequencer and explorer/indexer agree on canonical finalized block
      IDs/hashes; the current `d033...` / block 4035 mismatch is resolved or
      explicitly remains disputed and unused.
- [ ] Release transactions are constructed/submitted through the pinned official
      LEZ wallet path with real proofs, not a handcrafted raw RPC shortcut.
- [ ] Settlement program deployment resolves by exact hash and finalized block on
      `https://explorer.testnet.lez.logos.co` from a clean browser context.
- [ ] Every successful bond lock plus acceptance, rejection, expiry, and failed-
      delivery settlement transaction is independently explorer-validated.
- [ ] Spending-control and paid-A2A value-moving transactions are independently
      explorer-validated and reconcile with wallet/application state.
- [ ] Chunk 20F's read-only evidence gate and independent clean-clone audit pass;
      screenshots, JSON, sequencer, indexer, and explorer observations agree.
- [ ] Loadable Logos Core module; no upstream module modifications.
- [ ] Independent shielded identity/funds for each agent.
- [ ] Single-command headless deployment on a clean machine.
- [ ] Real-time encrypted owner channel from a separate Logos app.
- [ ] Atomic transaction and period spending limits with fail-closed approval.
- [ ] Storage: upload, download, list, share.
- [ ] Messaging: send, join, create group.
- [ ] Blockchain: balance, send, history, query, call, deploy.
- [ ] A2A: card, discover, task, subscribe, cancel.
- [ ] Meta: skills, status, configure.
- [ ] Third-party skill SDK works without core edits; failures are isolated.
- [ ] A2A cards and lifecycle conform to the pinned schema and documented binding.
- [ ] Autonomous paid task succeeds between at least two agents.
- [ ] Inbox, Vault, and Settlement profiles deployed separately on LEZ testnet.
- [ ] Three named LP-0008 illustrative use cases demonstrated end-to-end.
- [ ] Bond acceptance, rejection, expiry, and failed-delivery outcomes verified.
- [ ] Policy immutability, trusted bypass, rate limits, duplicate protection, and
      one encrypted attachment verified.
- [ ] Model output alone cannot confiscate funds.
- [ ] Restart/network/storage/owner-unavailable recovery verified.
- [ ] CU cost documented for every on-chain operation.
- [ ] Basecamp assets load and all owner workflows function.
- [ ] CI green on the default branch.
- [ ] Clean-clone real-proof demo succeeds with `RISC0_DEV_MODE=0`.
- [ ] Security/privacy/economic reviews have no unresolved critical/high issues.
- [ ] README, SDK, deployment, owner, A2A, security, limitations, and operations
      documentation complete.
- [ ] Narrated video, testnet evidence, clean public release, and submission draft
      complete and approved by the owner before publication.

## Explicitly Deferred Beyond the Submission MVP

Do not add these until the final gate is complete unless a verified LP-0008
requirement makes one necessary:

- Email and social-network bridges.
- Fiat payments.
- Public reputation scores.
- Community governance or configurable rejection proceeds to the inbox owner.
- Remote AI APIs enabled by default.
- Automatic financial penalties based only on classifier/model output.
- A standalone consumer UI beyond the required Basecamp integration.
