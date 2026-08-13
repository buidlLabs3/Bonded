# Known Limitations

- The official explorer's indexer is server-side and has no discovered public
  status/RPC endpoint. The verifier measures freshness by comparing its latest
  finalized blocks with sequencer blocks at the same heights.
- The production Core module binds the exact pinned Logos Delivery and Storage
  generated clients. Their adapter logic and module build pass locally, but live
  multi-process interoperability and fault-injection evidence are not collected.
  Local suites continue to use explicitly labelled memory test doubles.
- The production wallet and program skill adapters fail closed because Logos
  Core does not expose the required shielded LEZ wallet/program host API.
- The official Logos wallet module currently exposes a work-in-progress Ethereum
  API, not the required shielded LEZ NPK/ISK surface.
- The corrected settlement program is deployed through the pinned official
  wallet and passes its explorer-evidence row with two separated finalized
  observations and paired fresh-browser captures. Publication does not prove a
  completed bond lifecycle or an agent-profile deployment.
- Official-wallet standalone qualification currently passes deployment,
  malformed-program rejection, insufficient-funds rejection, and duplicate
  replay rejection. The real privacy-preserving transfer proof is still pending,
  so the artifact remains `qualification-in-progress`.
- The sender-registration transaction
  `2e64e58926ec14b4b59c477c7bd0f63a824e4f7122f8410ee9fd0a1ea14ba259`
  is finalized in block 4415 and passes the paired public-explorer evidence
  gate. The owner-registration transaction
  `9f8e1461b7f3a3dc805a392811d8849e67d65d099bde25c88639ff85f22c7c16`
  is finalized in block 4568 and also passes the paired public-explorer evidence
  gate. The sink-registration transaction
  `2f1df54d2658dc6ee45ae3eaac43902fcf548462cba7ff6599ca0670abc759ac`
  is finalized in block 4891 and passes the same gate. Sender funding transaction
  `05fb87d3eda87fad090eafd1b5cff7b001faf8df594eb81ba74ee00e5e4f9b15`
  is finalized in block 5661, passes the gate, and reconciles an exact 150-unit
  sender credit. Bond lifecycle and use-case transactions remain pending and
  must be validated separately.
- The official-wallet Bonded invocation adapter and sequential matrix runner are
  implemented and locally tested; the eight public lifecycle calls and their
  explorer evidence are not yet complete.
- Agent-profile deployments, bond invocation proofs, and CU measurements have
  not been collected.
- `scripts/e2e-real-proof.sh` intentionally exits nonzero even when
  `RISC0_DEV_MODE=0` until the real sequencer environment is provisioned.
- Basecamp QML assets have static checks but not live host/replica, screen-reader,
  localization, or viewport evidence.
- Third-party skills can be registered and isolated at build time; dynamic ABI
  loading waits for a stable upstream plugin contract.
- The CLI writes a service descriptor but does not download or start Logos Core.
- No narrated video, release tag, or Lambda Prize submission has been published.

These are release blockers where the final gate requires them, not hidden
successes. Do not use production identities or funds.
