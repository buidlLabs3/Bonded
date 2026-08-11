#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="${TMPDIR:-/tmp}/bonded-inbox-tests"

cd "$repo_root"
unset NIX_LDFLAGS LDFLAGS
read -r -a dependency_flags <<< "$(pkg-config --cflags --libs openssl sqlite3 nlohmann_json)"

g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Isrc \
    tests/test_main.cpp \
    src/domain/state_machine.cpp \
    src/domain/policy.cpp \
    src/security/crypto.cpp \
    src/storage/database.cpp \
    src/runtime/skill_registry.cpp \
    src/services/bond_service.cpp \
    src/services/inbox_service.cpp \
    "${dependency_flags[@]}" -pthread -o "$test_binary"

"$test_binary"
