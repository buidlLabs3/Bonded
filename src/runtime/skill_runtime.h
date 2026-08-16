#pragma once

#include "integrations/interfaces.h"
#include "runtime/skill_registry.h"
#include "services/a2a_service.h"
#include "services/configuration_service.h"
#include "services/messaging_service.h"
#include "services/spending_controller.h"
#include "services/storage_service.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace bonded {

class Database;

struct RuntimeAdapters {
    std::unique_ptr<MessagingAdapter> messaging;
    std::unique_ptr<StorageAdapter> storage;
    std::unique_ptr<WalletAdapter> wallet;
    std::unique_ptr<ProgramAdapter> program;
    std::string messaging_name;
    std::string storage_name;
    std::string wallet_name;
    std::string program_name;

    static RuntimeAdapters memory(std::uint64_t initial_balance);
};

class SkillRuntime {
public:
    using OwnerCommand = std::function<Json(const std::string&, const Json&)>;

    SkillRuntime(SkillRegistry& registry, Profile profile, const Json& configuration,
                 std::function<void(const Json&)> owner_action_required);
    SkillRuntime(SkillRegistry& registry, Profile profile, const Json& configuration,
                 std::function<void(const Json&)> owner_action_required,
                 RuntimeAdapters adapters, Database* database = nullptr);

    void registerDefaultSkills();
    Json status() const;
    Json ownerState() const;
    Json decideSpending(const std::string& proposal_id, bool approved,
                        std::uint64_t now_unix);
    Json updateOwnerConfiguration(const Json& changes, std::uint64_t expected_revision);
    void setOwnerCommandHandler(OwnerCommand handler);
    Json ownerAgents(std::uint64_t now_unix) const;
    Json requestOwnerCommand(const std::string& target_agent_id,
                             const std::string& action, const Json& payload,
                             std::uint64_t now_unix);
    Json ownerResponses() const;

private:
    Json handler(const std::string& name, const Json& input);
    AgentCard ownCard(std::uint64_t now_unix, std::uint64_t expires_at,
                      std::uint64_t task_price,
                      const std::string& payment_recipient) const;
    static Json spendingProposalJson(const SpendingProposal& proposal);
    static std::string ownerRequestTopic(const std::string& agent_id);
    static std::string ownerResponseTopic(const std::string& agent_id);
    void receiveOwnerRequest(const std::string& raw);
    void receiveOwnerResponse(const std::string& raw);

    SkillRegistry& registry_;
    Profile profile_;
    std::string network_;
    std::string agent_id_;
    std::string private_key_;
    std::string public_key_;
    std::string encryption_private_key_;
    std::string encryption_public_key_;
    std::string storage_key_;
    std::string owner_public_key_;
    std::function<void(const Json&)> owner_action_required_;
    OwnerCommand owner_command_;
    Database* database_{nullptr};
    mutable std::mutex owner_mutex_;
    std::map<std::string, AgentCard> owner_pending_;
    std::map<std::string, Json> owner_responses_;
    std::set<std::string> owner_processed_;

    std::unique_ptr<MessagingAdapter> messaging_adapter_;
    std::unique_ptr<StorageAdapter> storage_adapter_;
    std::unique_ptr<WalletAdapter> wallet_adapter_;
    std::unique_ptr<ProgramAdapter> program_adapter_;
    std::string messaging_adapter_name_;
    std::string storage_adapter_name_;
    std::string wallet_adapter_name_;
    std::string program_adapter_name_;
    MessagingService messaging_;
    StorageService storage_;
    SpendingController spending_;
    A2AService a2a_;
    ConfigurationService configuration_;
};

} // namespace bonded
