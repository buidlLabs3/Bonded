#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="${TMPDIR:-/tmp}/bonded-inbox-service-tests"

cd "$repo_root"
unset NIX_LDFLAGS LDFLAGS
read -r -a dependency_flags <<< "$(pkg-config --cflags --libs openssl sqlite3 nlohmann_json)"

g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Isrc \
    tests/test_services.cpp \
    src/domain/state_machine.cpp \
    src/security/crypto.cpp \
    src/integrations/memory_adapters.cpp \
    src/services/contact_rules.cpp \
    src/services/messaging_service.cpp \
    src/services/receipt_service.cpp \
    src/services/spending_controller.cpp \
    src/services/storage_service.cpp \
    "${dependency_flags[@]}" -pthread -o "$test_binary"

"$test_binary"
