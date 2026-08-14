# Contributing

Use disposable identities and testnet tokens. Never commit keys, wallet storage,
databases, generated `.lgx` files, raw wallet logs, or real message content.

## Development

```bash
nix develop
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
scripts/run-python-tests.sh
```

Run the focused Rust checks when changing settlement code:

```bash
cargo test --locked --manifest-path programs/bonded-inbox/Cargo.toml
cargo check --locked --manifest-path programs/bonded-inbox/lez-guest/Cargo.toml
```

Format C++ with `clang-format`. Add tests for changed behavior. Financial state
changes must cover duplicate, retry, crash, timeout, and conflicting terminal
decisions. Testnet evidence must come from the official wallet path and pass the
read-only evidence gate before documentation claims are updated.
