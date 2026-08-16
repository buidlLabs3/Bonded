#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="${TMPDIR:-/tmp}/bonded-inbox-skill-runtime-tests"

cd "$repo_root"
unset NIX_LDFLAGS LDFLAGS
read -r -a dependency_flags <<< "$(pkg-config --cflags --libs openssl sqlite3 nlohmann_json)"

g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Isrc \
    tests/test_skill_runtime.cpp \
    src/domain/state_machine.cpp \
    src/security/crypto.cpp \
    src/storage/database.cpp \
    src/integrations/memory_adapters.cpp \
    src/integrations/logos_adapters.cpp \
    src/runtime/default_skill_catalog.cpp \
    src/runtime/skill_registry.cpp \
    src/runtime/skill_runtime.cpp \
    src/services/a2a_service.cpp \
    src/services/configuration_service.cpp \
    src/services/messaging_service.cpp \
    src/services/spending_controller.cpp \
    src/services/storage_service.cpp \
    "${dependency_flags[@]}" -pthread -o "$test_binary"

"$test_binary"
