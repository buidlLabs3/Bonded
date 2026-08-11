#!/usr/bin/env bash
set -euo pipefail

if [[ "${RISC0_DEV_MODE:-}" != "0" ]]; then
    echo "RISC0_DEV_MODE must be exactly 0" >&2
    exit 2
fi

echo "Real-proof evaluator environment is not provisioned yet." >&2
echo "This fail-closed script will be completed after the pinned LEZ wrapper and sequencer are integrated." >&2
exit 3
