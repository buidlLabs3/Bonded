#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qml_runner="${BONDED_QML_RUNNER:-}"
qml_entry="$repo_root/basecamp/preview/Main.qml"

if [[ "${1:-}" == "--smoke" ]]; then
    qml_entry="$repo_root/basecamp/preview/Smoke.qml"
fi

if [[ -z "$qml_runner" ]]; then
    if command -v qml6 >/dev/null 2>&1; then
        qml_runner="$(command -v qml6)"
    elif command -v qml >/dev/null 2>&1; then
        qml_runner="$(command -v qml)"
    else
        echo "Qt's qml runner was not found. Set BONDED_QML_RUNNER to its absolute path." >&2
        exit 1
    fi
fi

if [[ ! -x "$qml_runner" ]]; then
    echo "BONDED_QML_RUNNER is not an executable file: $qml_runner" >&2
    exit 1
fi

qml_import_args=()
qml_prefix="$(cd "$(dirname "$qml_runner")/.." && pwd)"
for qml_import_root in "$qml_prefix/lib/qt-6/qml" "$qml_prefix/lib/qt6/qml"; do
    if [[ -d "$qml_import_root" ]]; then
        qml_import_args+=( -I "$qml_import_root" )
    fi
done

exec "$qml_runner" "${qml_import_args[@]}" -I "$repo_root/basecamp" "$qml_entry"
