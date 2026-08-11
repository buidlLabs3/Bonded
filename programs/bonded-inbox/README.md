# Bonded Inbox LEZ Program Core

This crate contains the allocation-free bond state machine and invariants. It is
kept independent from host code so the same transition logic can be wrapped by
the pinned LEZ guest/program ABI after the wallet FFI compatibility spike.

Run `cargo test --manifest-path programs/bonded-inbox/Cargo.toml` for host tests.
The release/evaluator gate additionally requires compiling the wrapper for the
pinned LEZ revision and running it with `RISC0_DEV_MODE=0`; host tests are not a
substitute for that evidence.
