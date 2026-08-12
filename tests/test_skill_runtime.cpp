#include "runtime/default_skill_catalog.h"
#include "runtime/skill_runtime.h"
#include "security/crypto.h"

#include <iostream>
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

        std::cout << "PASS 21-skill runtime conformance\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL 21-skill runtime conformance: " << error.what() << '\n';
        return 1;
    }
}
