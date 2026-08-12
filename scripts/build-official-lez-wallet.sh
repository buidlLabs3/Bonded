#!/usr/bin/env bash
set -euo pipefail

source_dir="${BONDED_LEZ_SOURCE:?set BONDED_LEZ_SOURCE to an external pinned LEZ checkout}"
expected_commit="47eba256479f6f785acbd138834340703cd03401"
expected_origin="https://github.com/logos-blockchain/logos-execution-zone.git"

if [[ "$(git -C "$source_dir" rev-parse HEAD)" != "$expected_commit" ]]; then
    echo "LEZ checkout is not at pinned commit $expected_commit" >&2
    exit 2
fi
origin="$(git -C "$source_dir" remote get-url origin)"
if [[ "$origin" != "$expected_origin" && \
      "$origin" != "${expected_origin%.git}" && \
      "$origin" != "git@github.com:logos-blockchain/logos-execution-zone.git" ]]; then
    echo "LEZ checkout origin is not the official repository" >&2
    exit 2
fi
if [[ -n "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]]; then
    echo "LEZ checkout has tracked modifications" >&2
    exit 2
fi

(
    cd "$source_dir"
    cargo build --locked --release -p wallet
)
printf '%s\n' "$source_dir/target/release/wallet"
