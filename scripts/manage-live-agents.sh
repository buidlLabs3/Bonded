#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
action="${1:-}"
shift || true

artifacts="$repo_root/build/live-stack"
data_root="$repo_root/build/live-agents"
owner_public_key=""
wallet_home=""
lez_account_id=""
lez_account_kind="private-owned"

usage() {
    echo "usage: $0 {deploy|start|stop|status|health} [options]" >&2
    echo "  --artifacts DIR --data-root DIR" >&2
    echo "  deploy: --owner-public-key HEX --wallet-home DIR --lez-account-id HEX" >&2
    exit 2
}

while (($#)); do
    case "$1" in
    --artifacts) artifacts="$2"; shift 2 ;;
    --data-root) data_root="$2"; shift 2 ;;
    --owner-public-key) owner_public_key="$2"; shift 2 ;;
    --wallet-home) wallet_home="$2"; shift 2 ;;
    --lez-account-id) lez_account_id="$2"; shift 2 ;;
    --lez-account-kind) lez_account_kind="$2"; shift 2 ;;
    *) usage ;;
    esac
done

case "$action" in
deploy|start|stop|status|health) ;;
*) usage ;;
esac

artifacts="$(realpath -m "$artifacts")"
data_root="$(realpath -m "$data_root")"
if [[ "$data_root" == / || "$data_root" == "$HOME" ]]; then
    echo "refusing broad data root: $data_root" >&2
    exit 2
fi

if [[ "$action" != deploy ]]; then
    for profile in inbox vault settlement; do
        "$repo_root/bin/bonded-inbox" --data-dir "$data_root/$profile" "$action"
    done
    exit 0
fi

if [[ ! "$owner_public_key" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "--owner-public-key must be a 32-byte hex Ed25519 public key" >&2
    exit 2
fi
if [[ ! "$lez_account_id" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "--lez-account-id must be a 32-byte hex LEZ account id" >&2
    exit 2
fi
if [[ "$lez_account_kind" != public && "$lez_account_kind" != private-owned ]]; then
    echo "--lez-account-kind must be public or private-owned" >&2
    exit 2
fi

one_file() {
    local root="$1"
    local pattern="$2"
    local found=()
    mapfile -t found < <(find -L "$root" -type f -name "$pattern" -print)
    if ((${#found[@]} != 1)); then
        echo "expected exactly one $pattern beneath $root" >&2
        exit 2
    fi
    echo "${found[0]}"
}

bonded_lgx="$(one_file "$artifacts/bonded-lgx" '*.lgx')"
delivery_lgx="$(one_file "$artifacts/delivery-lgx" '*.lgx')"
storage_lgx="$(one_file "$artifacts/storage-lgx" '*.lgx')"
lez_core_lgx="$(one_file "$artifacts/lez-core-lgx" '*.lgx')"
logoscore="$(one_file "$artifacts/logoscore-cli" logoscore)"
lgpm="$(one_file "$artifacts/lgpm-cli" lgpm)"
wallet_home="$(realpath "$wallet_home")"

for wallet_file in wallet_config.json storage.json statistics.json; do
    if [[ ! -f "$wallet_home/$wallet_file" ]]; then
        echo "missing wallet file: $wallet_home/$wallet_file" >&2
        exit 2
    fi
done

source_commit="$(git -C "$repo_root" rev-parse HEAD)"
common=(
    --network lez-testnet
    --owner-public-key "$owner_public_key"
    --source-commit "$source_commit"
    --module "$bonded_lgx"
    --dependency-module "$delivery_lgx"
    --dependency-module "$storage_lgx"
    --dependency-module "$lez_core_lgx"
    --core-binary "$logoscore"
    --package-manager "$lgpm"
)

for profile in inbox vault; do
    "$repo_root/bin/bonded-inbox" --data-dir "$data_root/$profile" deploy \
        --profile "$profile" "${common[@]}"
done

"$repo_root/bin/bonded-inbox" --data-dir "$data_root/settlement" deploy \
    --profile settlement "${common[@]}" \
    --lez-config "$wallet_home/wallet_config.json" \
    --lez-storage "$wallet_home/storage.json" \
    --lez-statistics "$wallet_home/statistics.json" \
    --lez-account-id "$lez_account_id" \
    --lez-account-kind "$lez_account_kind"
