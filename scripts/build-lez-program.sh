#!/usr/bin/env bash
set -euo pipefail

if [[ "${RISC0_DEV_MODE:-}" != "0" ]]; then
    echo "RISC0_DEV_MODE must be exactly 0" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

cargo test --locked --manifest-path programs/bonded-inbox/Cargo.toml
cargo check --locked --manifest-path programs/bonded-inbox/lez-guest/Cargo.toml
cargo risczero build --manifest-path programs/bonded-inbox/lez-guest/Cargo.toml

candidate="programs/bonded-inbox/lez-guest/target/riscv32im-risc0-zkvm-elf/docker/bonded_inbox.bin"
if [[ ! -f "$candidate" ]]; then
    echo "cargo-risczero did not produce the expected ELF: $candidate" >&2
    exit 3
fi

output="${BONDED_LEZ_ELF_OUT:-build/lez/bonded_inbox.bin}"
mkdir -p "$(dirname "$output")"
cp "$candidate" "$output"

program_id="$(r0vm --elf "$output" --id)"
sha256="$(sha256sum "$output" | cut -d' ' -f1)"
printf '{"binary":"%s","program_id":"%s","sha256":"%s"}\n' \
    "$output" "$program_id" "$sha256"
