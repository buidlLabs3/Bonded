# Contributing

## Development

1. Install Nix with flakes enabled.
2. Run `nix develop`.
3. Run `cmake --preset dev` and `cmake --build --preset dev`.
4. Run `ctest --preset dev` before proposing a change.

Use only disposable development identities and testnet tokens. Never commit
generated keys, databases, `.lgx` packages, or message fixtures containing real
content.

Code must be formatted with `clang-format`, compile without new warnings, and
include tests for changed behavior. Financial state transitions require tests
for duplicates, retries, crashes, and conflicting terminal decisions.
