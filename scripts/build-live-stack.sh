#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_root="${1:-$repo_root/build/live-stack}"

mkdir -p "$output_root"

build() {
    local installable="$1"
    local link_name="$2"
    nix --extra-experimental-features 'nix-command flakes' \
        build --accept-flake-config "$installable" \
        --out-link "$output_root/$link_name"
}

build "$repo_root#lgx" bonded-lgx
build "$repo_root#basecamp-lgx" basecamp-lgx
build github:logos-co/logos-delivery-module/3f0f2d8b202f427a96179407bbf18b449935da7c#lgx delivery-lgx
build github:logos-co/logos-storage-module/e0db835de379f47bae7fccc3032056d99af973bb#lgx storage-lgx
build github:logos-blockchain/logos-execution-zone-module/cd47b9e137991a3045aceb88f62de4c9db3fcb44#lgx lez-core-lgx
build github:logos-co/logos-logoscore-cli/1a4f56fe2b5fe3ed34d7063a4b7376627c8cb838#cli logoscore-cli
build github:logos-co/logos-package-manager/3ceabe25a2d4733c23517099e0fa0f11ea8861eb#cli lgpm-cli

find -L "$output_root" -maxdepth 3 -type f \
    \( -name '*.lgx' -o -name logoscore -o -name lgpm \) -print | sort
