#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
python3 -m unittest -v \
    tests/test_cli.py \
    tests/test_assets.py \
    tests/test_lez_testnet.py \
    tests/test_lez_explorer.py \
    tests/test_lez_evidence_gate.py \
    tests/test_lez_wallet.py \
    tests/test_lez_standalone.py \
    tests/test_lez_bond.py \
    tests/test_lez_wallet_bootstrap.py \
    tests/test_lez_wallet_provision.py
