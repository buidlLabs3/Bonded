cmake_minimum_required(VERSION 3.20)
project(BondedInbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found; use nix develop or nix build")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)

set(BONDED_SOURCES
    src/bonded_inbox_impl.h
    src/bonded_inbox_impl.cpp
    src/domain/state_machine.cpp
    src/domain/policy.cpp
    src/security/crypto.cpp
    src/storage/database.cpp
    src/integrations/memory_adapters.cpp
    src/integrations/logos_adapters.cpp
    src/runtime/default_skill_catalog.cpp
    src/runtime/reliability.cpp
    src/runtime/skill_registry.cpp
    src/runtime/skill_runtime.cpp
    src/services/a2a_service.cpp
    src/services/bond_service.cpp
    src/services/configuration_service.cpp
    src/services/inbox_service.cpp
    src/services/contact_rules.cpp
    src/services/messaging_service.cpp
    src/services/reconciler.cpp
    src/services/receipt_service.cpp
    src/services/spending_controller.cpp
    src/services/storage_service.cpp
    src/services/triage_service.cpp
)

logos_module(NAME ${MODULE_NAME} SOURCES ${BONDED_SOURCES})

target_include_directories(${MODULE_NAME}_module_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(${MODULE_NAME}_module_plugin PRIVATE OpenSSL::Crypto SQLite::SQLite3
                                                    nlohmann_json::nlohmann_json)

include(CTest)
if(BUILD_TESTING)
    add_executable(bonded_inbox_tests
        tests/test_main.cpp
        src/domain/state_machine.cpp
        src/domain/policy.cpp
        src/security/crypto.cpp
        src/storage/database.cpp
        src/runtime/skill_registry.cpp
        src/services/bond_service.cpp
        src/services/inbox_service.cpp
    )
    target_include_directories(bonded_inbox_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(bonded_inbox_tests PRIVATE OpenSSL::Crypto SQLite::SQLite3
                                                        nlohmann_json::nlohmann_json)
    add_test(NAME bonded_inbox_tests COMMAND bonded_inbox_tests)

    add_executable(bonded_inbox_service_tests
        tests/test_services.cpp
        src/domain/state_machine.cpp
        src/security/crypto.cpp
        src/integrations/memory_adapters.cpp
        src/integrations/logos_adapters.cpp
        src/services/contact_rules.cpp
        src/services/messaging_service.cpp
        src/services/receipt_service.cpp
        src/services/spending_controller.cpp
        src/services/storage_service.cpp
    )
    target_include_directories(bonded_inbox_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(bonded_inbox_service_tests PRIVATE OpenSSL::Crypto SQLite::SQLite3
                                                                nlohmann_json::nlohmann_json)
    add_test(NAME bonded_inbox_service_tests COMMAND bonded_inbox_service_tests)

    add_executable(bonded_inbox_second_half_tests
        tests/test_second_half.cpp
        src/security/crypto.cpp
        src/integrations/memory_adapters.cpp
        src/integrations/logos_adapters.cpp
        src/runtime/default_skill_catalog.cpp
        src/runtime/reliability.cpp
        src/runtime/skill_registry.cpp
        src/services/a2a_service.cpp
        src/services/configuration_service.cpp
        src/services/messaging_service.cpp
        src/services/triage_service.cpp
    )
    target_include_directories(bonded_inbox_second_half_tests
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(bonded_inbox_second_half_tests PRIVATE OpenSSL::Crypto SQLite::SQLite3
                                                                    nlohmann_json::nlohmann_json)
    add_test(NAME bonded_inbox_second_half_tests COMMAND bonded_inbox_second_half_tests)

    add_executable(bonded_inbox_skill_runtime_tests
        tests/test_skill_runtime.cpp
        src/domain/state_machine.cpp
        src/security/crypto.cpp
        src/storage/database.cpp
        src/integrations/memory_adapters.cpp
        src/runtime/default_skill_catalog.cpp
        src/runtime/skill_registry.cpp
        src/runtime/skill_runtime.cpp
        src/services/a2a_service.cpp
        src/services/configuration_service.cpp
        src/services/messaging_service.cpp
        src/services/spending_controller.cpp
        src/services/storage_service.cpp
    )
    target_include_directories(bonded_inbox_skill_runtime_tests
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(bonded_inbox_skill_runtime_tests PRIVATE OpenSSL::Crypto SQLite::SQLite3
                                                                     nlohmann_json::nlohmann_json)
    add_test(NAME bonded_inbox_skill_runtime_tests COMMAND bonded_inbox_skill_runtime_tests)

    add_executable(bonded_inbox_local_benchmark
        benchmarks/local_workloads.cpp
        src/domain/state_machine.cpp
        src/security/crypto.cpp
        src/storage/database.cpp
        src/integrations/memory_adapters.cpp
        src/runtime/reliability.cpp
        src/services/contact_rules.cpp
        src/services/a2a_service.cpp
        src/services/bond_service.cpp
    )
    target_include_directories(bonded_inbox_local_benchmark
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(bonded_inbox_local_benchmark PRIVATE OpenSSL::Crypto SQLite::SQLite3
                                                               Threads::Threads
                                                               nlohmann_json::nlohmann_json)
endif()
