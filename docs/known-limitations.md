# Known Limitations

- The official explorer's indexer is server-side and has no discovered public
  status/RPC endpoint. The verifier measures freshness by comparing its latest
  finalized blocks with sequencer blocks at the same heights.
- The checked-in Messaging, Storage, wallet, and program adapters are in-memory
  test doubles. Official upstream adapters are not yet bound.
- The official Logos wallet module currently exposes a work-in-progress Ethereum
  API, not the required shielded LEZ NPK/ISK surface.
- Program deployment is explorer-validated, but it used the direct sequencer
  client predating Chunk 20B; it is not evidence of an official wallet call path.
- The real LEZ guest and program-deployment path do not yet provide a wallet/Core
  invocation adapter or an end-to-end bond execution proof.
- Three independent testnet identities, agent-profile deployments, bond
  invocation proofs, and CU measurements have not been collected.
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
