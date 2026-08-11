# Known Limitations

- The checked-in Messaging, Storage, wallet, and program adapters are in-memory
  test doubles. Official upstream adapters are not yet bound.
- The official Logos wallet module currently exposes a work-in-progress Ethereum
  API, not the required shielded LEZ NPK/ISK surface.
- The Rust bond core is allocation-free logic, not a deployed LEZ guest wrapper.
- Testnet identities, program IDs, transactions, proofs, CU measurements, and
  three-profile deployment evidence have not been collected.
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
