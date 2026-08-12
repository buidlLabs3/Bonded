#!/usr/bin/env bash
set -euo pipefail

if [[ "${RISC0_DEV_MODE:-}" != "0" ]]; then
    echo "RISC0_DEV_MODE must be exactly 0" >&2
    exit 2
fi
if [[ "${BONDED_LEZ_SUBMIT:-}" != "YES" ]]; then
    echo "set BONDED_LEZ_SUBMIT=YES to authorize official-wallet submission" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

python3 tools/lez_wallet.py deploy-program \
    --elf "${BONDED_LEZ_ELF:-build/lez/bonded_inbox.bin}" \
    --wallet-source "${BONDED_LEZ_SOURCE:?set BONDED_LEZ_SOURCE}" \
    --wallet-home "${BONDED_LEZ_WALLET_HOME:?set BONDED_LEZ_WALLET_HOME outside this repository}" \
    --submit \
    --evidence "${BONDED_LEZ_EVIDENCE:-evidence/testnet/candidates/official-wallet-program.json}"
