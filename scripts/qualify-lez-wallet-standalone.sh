#!/usr/bin/env bash
set -euo pipefail

if [[ "${RISC0_DEV_MODE:-}" != "0" ]]; then
    echo "RISC0_DEV_MODE must be exactly 0" >&2
    exit 2
fi
if [[ "${BONDED_LEZ_STANDALONE:-}" != "YES" ]]; then
    echo "set BONDED_LEZ_STANDALONE=YES to authorize the Docker-backed qualification" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

python3 tools/lez_standalone.py \
    --wallet-source "${BONDED_LEZ_SOURCE:?set BONDED_LEZ_SOURCE}" \
    --run \
    --evidence "${BONDED_LEZ_STANDALONE_EVIDENCE:-evidence/standalone/official-wallet-real-proof.json}" \
    "$@"
