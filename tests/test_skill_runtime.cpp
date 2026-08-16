#include "runtime/default_skill_catalog.h"
#include "runtime/skill_runtime.h"
#include "integrations/memory_adapters.h"
#include "security/crypto.h"

#include <iostream>
#include <ctime>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>

namespace {

using bonded::Json;
using bonded::Profile;
using bonded::SkillRegistry;
using bonded::SkillRuntime;

class SharedMessagingAdapter final : public bonded::MessagingAdapter {
public:
    explicit SharedMessagingAdapter(std::shared_ptr<bonded::MemoryMessagingAdapter> bus)
        : bus_(std::move(bus)) {}

    std::string send(const std::string& topic, const std::string& payload) override
    {
        return bus_->send(topic, payload);
    }

    void subscribe(const std::string& topic,
                   std::function<void(const std::string&)> handler) override
    {
        bus_->subscribe(topic, std::move(handler));
    }

    void join(const std::string& group_id) override { bus_->join(group_id); }

    std::string createGroup(const std::vector<std::string>& members) override
    {
        return bus_->createGroup(members);
    }

private:
    std::shared_ptr<bonded::MemoryMessagingAdapter> bus_;
};

bonded::RuntimeAdapters sharedAdapters(
    const std::shared_ptr<bonded::MemoryMessagingAdapter>& bus)
{
    return {std::make_unique<SharedMessagingAdapter>(bus),
            std::make_unique<bonded::MemoryStorageAdapter>(),
            std::make_unique<bonded::MemoryWalletAdapter>(1000),
            std::make_unique<bonded::MemoryProgramAdapter>(),
            "logos-delivery-module", "logos-storage-module",
            "memory-test-double", "memory-test-double"};
}

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

        const auto card = registry.invoke(
            "agent.card", Profile::Vault,
            Json{{"now_unix", 100},
                 {"expires_at", 1000},
                 {"task_price", 5},
                 {"payment_recipient", std::string(64, 'a')},
                 {"payment_private_keys",
                  Json{{"nullifier_public_key", std::string(64, 'b')},
                       {"viewing_public_key", std::string(64, 'c')}}}});
        check(card.at("supportedInterfaces").at(0).at("protocolVersion") == "1.0" &&
                  card.at("supportedInterfaces").at(0).at("protocolBinding") ==
                      "LOGOS-MESSAGING" &&
                  card.at("signatures").at(0).contains("protected") &&
                  card.at("signatures").at(0).contains("signature"),
              "A2A 1.0 Agent Card interface or JWS signature is missing");
        check(card.at("capabilities").at("extensions").at(0).at("params")
                      .at("encryptionPublicKey").get<std::string>().size() == 64,
              "Agent Card omitted the X25519 messaging key");
        check(card.at("capabilities").at("extensions").at(1).at("params")
                      .at("recipientPrivateKeys").at("nullifier_public_key") ==
                  std::string(64, 'b'),
              "Agent Card omitted the LEZ private recipient keys");
        check(registry.invoke("agent.discover", Profile::Vault,
                              Json{{"now_unix", 100}, {"skill", "storage.upload"}})
                      .size() == 1,
              "A2A discovery skill did not return the local card");
        const auto task = registry.invoke(
            "agent.task", Profile::Vault,
            Json{{"task_id", "task-1"},
                 {"provider", card.at("name")},
                 {"skill", "storage.upload"},
                 {"input", Json{{"label", "demo"}}},
                 {"price", 5},
                 {"expires_at", 900},
                 {"now_unix", 100}});
        check(task.at("status").at("state") == "TASK_STATE_WORKING" &&
                  task.at("metadata").at("logos").at("paymentReference") == "",
              "A2A task claimed a nonexistent escrow payment");
        const auto completed = registry.invoke(
            "agent.task", Profile::Vault,
            Json{{"action", "complete"},
                 {"task_id", "task-1"},
                 {"now_unix", 101},
                 {"output", Json{{"address", "sha256:result"}}}});
        check(completed.at("status").at("state") == "TASK_STATE_COMPLETED" &&
                  completed.at("artifacts").at(0).at("parts").at(0).at("data")
                      .at("address") == "sha256:result",
              "A2A task did not complete without fake escrow calls");
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

        const auto owner_bus = std::make_shared<bonded::MemoryMessagingAdapter>();
        SkillRegistry controller_registry;
        SkillRuntime controller(controller_registry, Profile::Inbox,
                                Json{{"network", "logos-local"}},
                                [](const Json&) {}, sharedAdapters(owner_bus));
        controller.registerDefaultSkills();
        const auto controller_status = controller.status();
        SkillRegistry agent_registry;
        SkillRuntime controlled_agent(
            agent_registry, Profile::Inbox,
            Json{{"network", "logos-local"},
                 {"owner_public_key", controller_status.at("signing_public_key")}},
            [](const Json&) {}, sharedAdapters(owner_bus));
        controlled_agent.registerDefaultSkills();
        controlled_agent.setOwnerCommandHandler(
            [](const std::string& action, const Json& payload) {
                check(action == "state.get", "owner channel changed the requested action");
                return Json{{"runtime", Json{{"state", "ready"}}},
                            {"echo", payload}};
            });
        const auto now = static_cast<std::uint64_t>(std::time(nullptr));
        const auto controlled_card = agent_registry.invoke(
            "agent.card", Profile::Inbox,
            Json{{"now_unix", now}, {"expires_at", now + 3600}});
        const auto request = controller.requestOwnerCommand(
            controlled_card.at("name").get<std::string>(), "state.get",
            Json{{"refresh", true}}, now);
        const auto responses = controller.ownerResponses();
        check(request.at("state") == "pending" && responses.size() == 1 &&
                  responses.at(0).at("requestId") == request.at("requestId") &&
                  responses.at(0).at("ok") == true &&
                  responses.at(0).at("result").at("runtime").at("state") == "ready" &&
                  responses.at(0).at("result").at("echo").at("refresh") == true,
              "encrypted owner channel did not complete across separate runtimes");

        const auto unauthorized_keys = bonded::Crypto::generateEd25519KeyPair();
        SkillRegistry unauthorized_registry;
        SkillRuntime unauthorized_agent(
            unauthorized_registry, Profile::Inbox,
            Json{{"network", "logos-local"},
                 {"owner_public_key", unauthorized_keys.second}},
            [](const Json&) {}, sharedAdapters(owner_bus));
        unauthorized_agent.registerDefaultSkills();
        unauthorized_agent.setOwnerCommandHandler(
            [](const std::string&, const Json&) {
                return Json{{"unauthorized", true}};
            });
        const auto unauthorized_card = unauthorized_registry.invoke(
            "agent.card", Profile::Inbox,
            Json{{"now_unix", now}, {"expires_at", now + 3600}});
        controller.requestOwnerCommand(
            unauthorized_card.at("name").get<std::string>(), "state.get",
            Json::object(), now);
        check(controller.ownerResponses().size() == 1,
              "agent accepted an owner channel command signed by the wrong controller");

        std::cout << "PASS 21-skill runtime conformance\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL 21-skill runtime conformance: " << error.what() << '\n';
        return 1;
    }
}
