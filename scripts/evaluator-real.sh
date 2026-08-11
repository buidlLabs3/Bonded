#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ "${RISC0_DEV_MODE:-}" != "0" ]]; then
    echo "RISC0_DEV_MODE must be exactly 0" >&2
    exit 2
fi

for required in LEZ_SEQUENCER_URL LOGOS_CORE_BIN BONDED_LGX; do
    if [[ -z "${!required:-}" ]]; then
        echo "missing evaluator input: $required" >&2
        exit 2
    fi
done

if [[ ! -x "$LOGOS_CORE_BIN" || ! -f "$BONDED_LGX" ]]; then
    echo "LOGOS_CORE_BIN must be executable and BONDED_LGX must exist" >&2
    exit 2
fi

for profile in inbox vault settlement; do
    evidence="evidence/testnet/${profile}.json"
    if [[ ! -f "$evidence" ]] || ! grep -q '"status": "verified"' "$evidence"; then
        echo "verified testnet evidence is missing for profile: $profile" >&2
        exit 3
    fi
done

exec ./scripts/e2e-real-proof.sh
