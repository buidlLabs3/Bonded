#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="$(mktemp -d "${TMPDIR:-/tmp}/bonded-inbox-tests.XXXXXX")"
test_binary="$build_root/bonded-inbox-tests"
compiler_pids=()

cleanup() {
    for compiler_pid in "${compiler_pids[@]}"; do
        if kill -0 "$compiler_pid" 2>/dev/null; then
            kill "$compiler_pid" 2>/dev/null || true
            wait "$compiler_pid" 2>/dev/null || true
        fi
    done
    rm -rf -- "$build_root"
}
trap cleanup EXIT

cd "$repo_root"
unset NIX_LDFLAGS LDFLAGS
read -r -a dependency_cflags <<< "$(pkg-config --cflags openssl sqlite3 nlohmann_json)"
read -r -a dependency_libs <<< "$(pkg-config --libs openssl sqlite3 nlohmann_json)"
compile_flags=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -Isrc "${dependency_cflags[@]}")
sources=(
    tests/test_main.cpp \
    src/domain/state_machine.cpp \
    src/domain/policy.cpp \
    src/security/crypto.cpp \
    src/storage/database.cpp \
    src/runtime/skill_registry.cpp \
    src/services/bond_service.cpp \
    src/services/inbox_service.cpp
)
objects=()
for index in "${!sources[@]}"; do
    object="$build_root/$index.o"
    objects+=("$object")
    g++ "${compile_flags[@]}" -c "${sources[$index]}" -o "$object" &
    compiler_pids+=("$!")
    if [[ "${#compiler_pids[@]}" -eq 2 ]]; then
        wait "${compiler_pids[0]}"
        wait "${compiler_pids[1]}"
        compiler_pids=()
    fi
done
for compiler_pid in "${compiler_pids[@]}"; do
    wait "$compiler_pid"
done
compiler_pids=()
g++ "${objects[@]}" "${dependency_libs[@]}" -pthread -o "$test_binary"

"$test_binary"
