#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
action="${1:-}"
shift || true

artifacts="$repo_root/build/live-stack"
data_root="$repo_root/build/live-agents"
owner_public_key=""
declare -A wallet_homes=([inbox]="" [vault]="" [settlement]="")
declare -A account_ids=([inbox]="" [vault]="" [settlement]="")

usage() {
    echo "usage: $0 {deploy|start|stop|status|health} [options]" >&2
    echo "  --artifacts DIR --data-root DIR" >&2
    echo "  deploy: --owner-public-key HEX" >&2
    echo "    --wallet-home-inbox DIR --lez-account-id-inbox HEX" >&2
    echo "    --wallet-home-vault DIR --lez-account-id-vault HEX" >&2
    echo "    --wallet-home-settlement DIR --lez-account-id-settlement HEX" >&2
    exit 2
}

while (($#)); do
    case "$1" in
    --artifacts) artifacts="$2"; shift 2 ;;
    --data-root) data_root="$2"; shift 2 ;;
    --owner-public-key) owner_public_key="$2"; shift 2 ;;
    --wallet-home-inbox) wallet_homes[inbox]="$2"; shift 2 ;;
    --wallet-home-vault) wallet_homes[vault]="$2"; shift 2 ;;
    --wallet-home-settlement) wallet_homes[settlement]="$2"; shift 2 ;;
    --lez-account-id-inbox) account_ids[inbox]="${2,,}"; shift 2 ;;
    --lez-account-id-vault) account_ids[vault]="${2,,}"; shift 2 ;;
    --lez-account-id-settlement) account_ids[settlement]="${2,,}"; shift 2 ;;
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
    if [[ "$action" == start ]]; then
        for profile in inbox vault settlement inbox vault; do
            "$repo_root/bin/bonded-inbox" --data-dir "$data_root/$profile" card
        done
    fi
    exit 0
fi

if [[ ! "$owner_public_key" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "--owner-public-key must be a 32-byte hex Ed25519 public key" >&2
    exit 2
fi
for profile in inbox vault settlement; do
    if [[ ! "${account_ids[$profile]}" =~ ^[0-9a-f]{64}$ ]]; then
        echo "--lez-account-id-$profile must be a 32-byte hex LEZ account id" >&2
        exit 2
    fi
    if [[ -z "${wallet_homes[$profile]}" ]]; then
        echo "--wallet-home-$profile is required" >&2
        exit 2
    fi
    wallet_homes[$profile]="$(realpath "${wallet_homes[$profile]}")"
    for wallet_file in wallet_config.json storage.json statistics.json; do
        if [[ ! -f "${wallet_homes[$profile]}/$wallet_file" ]]; then
            echo "missing $profile wallet file: ${wallet_homes[$profile]}/$wallet_file" >&2
            exit 2
        fi
    done
done
if [[ "${account_ids[inbox]}" == "${account_ids[vault]}" ||
      "${account_ids[inbox]}" == "${account_ids[settlement]}" ||
      "${account_ids[vault]}" == "${account_ids[settlement]}" ]]; then
    echo "agent LEZ account IDs must be distinct" >&2
    exit 2
fi
if [[ "${wallet_homes[inbox]}" == "${wallet_homes[vault]}" ||
      "${wallet_homes[inbox]}" == "${wallet_homes[settlement]}" ||
      "${wallet_homes[vault]}" == "${wallet_homes[settlement]}" ]]; then
    echo "agent wallet homes must be distinct" >&2
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

for profile in inbox vault settlement; do
    "$repo_root/bin/bonded-inbox" --data-dir "$data_root/$profile" deploy \
        --profile "$profile" "${common[@]}" \
        --lez-config "${wallet_homes[$profile]}/wallet_config.json" \
        --lez-storage "${wallet_homes[$profile]}/storage.json" \
        --lez-statistics "${wallet_homes[$profile]}/statistics.json" \
        --lez-account-id "${account_ids[$profile]}" \
        --lez-account-kind private-owned
done

# Publish after every Core instance is subscribed, then repeat free cards so late
# subscribers receive the full three-agent catalog without retained-topic assumptions.
for profile in inbox vault settlement inbox vault; do
    "$repo_root/bin/bonded-inbox" --data-dir "$data_root/$profile" card
done
