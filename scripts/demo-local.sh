#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

./scripts/run-unit-tests.sh
./scripts/run-service-tests.sh
./scripts/run-second-half-tests.sh
./scripts/run-skill-runtime-tests.sh
./scripts/run-python-tests.sh
cargo test --manifest-path programs/bonded-inbox/Cargo.toml

printf '%s\n' '{"ok":true,"mode":"local-adapters","real_proof":false,"testnet":false}'
