#include "runtime/default_skill_catalog.h"
#include "runtime/skill_runtime.h"
#include "integrations/memory_adapters.h"
#include "security/crypto.h"

#include <iostream>
#include <filesystem>
#include <set>
#include <stdexcept>

namespace {

using bonded::Json;
using bonded::Profile;
using bonded::SkillRegistry;
using bonded::SkillRuntime;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        SkillRegistry registry;
        SkillRuntime runtime(registry, Profile::Vault,
                             Json{{"network", "logos-local"},
                                  {"initial_balance", 1000},
                                  {"per_transaction_limit", 100},
                                  {"per_period_limit", 500}},
                             [](const Json&) {});
        runtime.registerDefaultSkills();
        check(registry.size() == 21, "runtime did not register exactly 21 default skills");

        std::set<std::string> union_names;
        for (const auto profile : {Profile::Inbox, Profile::Vault, Profile::Settlement}) {
            for (const auto& item : registry.manifest(profile)) {
                union_names.insert(item.at("name").get<std::string>());
            }
        }
        check(union_names == bonded::requiredDefaultSkillNames(),
              "profile manifests do not cover the required default skill set");

        const auto uploaded = registry.invoke(
            "storage.upload", Profile::Vault, Json{{"plaintext", "private"}, {"label", "note"}});
        const auto downloaded = registry.invoke(
            "storage.download", Profile::Vault, Json{{"address", uploaded.at("address")}});
        check(downloaded.at("plaintext") == "private", "storage skill round trip failed");
        check(registry.invoke("wallet.balance", Profile::Settlement, Json::object()).at("balance") ==
                  1000,
              "wallet balance skill failed");
        check(registry.invoke("messaging.create_group", Profile::Inbox,
                              Json{{"members", Json::array({"peer"})}})
                      .contains("group_id"),
              "messaging group skill failed");
        const auto recipient_encryption_keys = bonded::Crypto::generateX25519KeyPair();
        const auto sent = registry.invoke(
            "messaging.send", Profile::Inbox,
            Json{{"recipient", "peer"},
                 {"topic", "bonded/private/channel"},
                 {"payload", "private skill payload"},
                 {"recipient_encryption_public_key", recipient_encryption_keys.second},
                 {"now_unix", 100}});
        check(sent.at("message_id").get<std::string>().starts_with("memory-message-"),
              "messaging send skill did not seal and dispatch an envelope");

        const auto card = registry.invoke("agent.card", Profile::Vault,
                                          Json{{"now_unix", 100}, {"expires_at", 1000}});
        check(card.at("protocol") == "lf.a2a.v1", "A2A card skill returned wrong protocol");
        check(card.at("capabilities").at("messaging_encryption") ==
                  "x25519-aes-256-gcm" &&
                  card.at("capabilities").at("messaging_encryption_public_key")
                          .get<std::string>()
                          .size() == 64,
              "Agent Card omitted the X25519 messaging key");
        check(registry.invoke("agent.discover", Profile::Vault,
                              Json{{"now_unix", 100}, {"skill", "storage.upload"}})
                      .size() == 1,
              "A2A discovery skill did not return the local card");
        const auto status = registry.invoke("meta.status", Profile::Vault, Json::object());
        check(status.at("state") == "ready",
              "meta status skill failed");
        check(status.at("messaging_encryption_public_key").get<std::string>().size() == 64,
              "runtime status omitted the X25519 messaging key");

        const auto identity_directory = std::filesystem::temp_directory_path() /
                                        ("bonded-identity-" + bonded::Crypto::randomHex(8));
        const Json persistent_configuration{{"network", "lez-testnet"},
                                            {"data_directory", identity_directory.string()}};
        std::string first_agent_id;
        std::string first_encryption_key;
        {
            SkillRegistry first_registry;
            SkillRuntime first_runtime(first_registry, Profile::Vault,
                                       persistent_configuration, [](const Json&) {});
            const auto first_status = first_runtime.status();
            first_agent_id = first_status.at("agent_id").get<std::string>();
            first_encryption_key =
                first_status.at("messaging_encryption_public_key").get<std::string>();
        }
        {
            SkillRegistry second_registry;
            SkillRuntime second_runtime(second_registry, Profile::Vault,
                                        persistent_configuration, [](const Json&) {});
            const auto second_status = second_runtime.status();
            check(second_status.at("agent_id") == first_agent_id &&
                      second_status.at("messaging_encryption_public_key") ==
                          first_encryption_key,
                  "runtime identity changed across restart");
        }
        const auto identity_path = identity_directory / "identity.json";
        check((std::filesystem::status(identity_path).permissions() &
               std::filesystem::perms::group_all) == std::filesystem::perms::none &&
                  (std::filesystem::status(identity_path).permissions() &
                   std::filesystem::perms::others_all) == std::filesystem::perms::none,
              "runtime identity is not owner-only");
        std::filesystem::remove_all(identity_directory);

        SkillRegistry injected_registry;
        bonded::RuntimeAdapters adapters{
            std::make_unique<bonded::MemoryMessagingAdapter>(),
            std::make_unique<bonded::MemoryStorageAdapter>(),
            std::make_unique<bonded::MemoryWalletAdapter>(25),
            std::make_unique<bonded::MemoryProgramAdapter>(),
            "logos-delivery-module",
            "logos-storage-module",
            "official-lez-wallet-pending-host-api",
            "official-lez-wallet-pending-host-api"};
        SkillRuntime injected_runtime(injected_registry, Profile::Settlement,
                                      Json{{"network", "lez-testnet"}},
                                      [](const Json&) {}, std::move(adapters));
        injected_runtime.registerDefaultSkills();
        const auto injected_status = injected_runtime.status();
        check(injected_status.at("dependencies").at("messaging") ==
                  "logos-delivery-module" &&
                  injected_status.at("dependencies").at("storage") ==
                      "logos-storage-module",
              "runtime did not report injected official dependencies");
        check(injected_status.at("dependencies").at("wallet") ==
                  "official-lez-wallet-pending-host-api",
              "runtime hid the official wallet host API gap");
        check(injected_status.at("balance") == 25 &&
                  injected_status.at("wallet_error").get<std::string>().empty(),
              "available injected wallet was reported as unavailable");

        SkillRegistry unavailable_registry;
        bonded::RuntimeAdapters unavailable_adapters{
            std::make_unique<bonded::MemoryMessagingAdapter>(),
            std::make_unique<bonded::MemoryStorageAdapter>(),
            std::make_unique<bonded::MemoryWalletAdapter>(0),
            std::make_unique<bonded::MemoryProgramAdapter>(),
            "logos-delivery-module",
            "logos-storage-module",
            "official-lez-wallet-host-api-unavailable",
            "official-lez-program-host-api-unavailable"};
        SkillRuntime unavailable_runtime(unavailable_registry, Profile::Vault,
                                         Json{{"network", "lez-testnet"}},
                                         [](const Json&) {},
                                         std::move(unavailable_adapters));
        check(unavailable_runtime.status().at("state") == "degraded",
              "Vault hid the unavailable LEZ program dependency");

        std::cout << "PASS 21-skill runtime conformance\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL 21-skill runtime conformance: " << error.what() << '\n';
        return 1;
    }
}
