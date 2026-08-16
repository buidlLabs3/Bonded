#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="${TMPDIR:-/tmp}/bonded-inbox-second-half-tests"

cd "$repo_root"
unset NIX_LDFLAGS LDFLAGS
read -r -a dependency_flags <<< "$(pkg-config --cflags --libs openssl sqlite3 nlohmann_json)"

g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Isrc \
    tests/test_second_half.cpp \
    src/security/crypto.cpp \
    src/integrations/memory_adapters.cpp \
    src/runtime/default_skill_catalog.cpp \
    src/runtime/reliability.cpp \
    src/runtime/skill_registry.cpp \
    src/services/a2a_service.cpp \
    src/services/configuration_service.cpp \
    src/services/messaging_service.cpp \
    src/services/triage_service.cpp \
    "${dependency_flags[@]}" -pthread -o "$test_binary"

"$test_binary"
