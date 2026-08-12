#!/usr/bin/env bash
set -euo pipefail

if [[ "${RISC0_DEV_MODE:-}" != "0" ]]; then
    echo "RISC0_DEV_MODE must be exactly 0" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

elf="${BONDED_LEZ_ELF:-build/lez/bonded_inbox.bin}"
if [[ ! -f "$elf" ]]; then
    echo "missing LEZ guest ELF: $elf (run scripts/build-lez-program.sh)" >&2
    exit 2
fi

release_commit="${BONDED_RELEASE_COMMIT:-$(git rev-parse HEAD)}"
exec python3 tools/lez_testnet.py \
    --endpoint "${LEZ_SEQUENCER_URL:-https://testnet.lez.logos.co}" \
    deploy \
    --elf "$elf" \
    --release-commit "$release_commit" \
    --evidence "${BONDED_LEZ_EVIDENCE:-evidence/testnet/settlement-program.json}"
