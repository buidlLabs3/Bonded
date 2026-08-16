#pragma once

#include "integrations/interfaces.h"
#include "runtime/skill_registry.h"
#include "services/a2a_service.h"
#include "services/configuration_service.h"
#include "services/messaging_service.h"
#include "services/spending_controller.h"
#include "services/storage_service.h"

#include <functional>
#include <memory>
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

private:
    Json handler(const std::string& name, const Json& input);
    AgentCard ownCard(std::uint64_t now_unix, std::uint64_t expires_at,
                      std::uint64_t task_price,
                      const std::string& payment_recipient) const;
    static Json spendingProposalJson(const SpendingProposal& proposal);

    SkillRegistry& registry_;
    Profile profile_;
    std::string network_;
    std::string agent_id_;
    std::string private_key_;
    std::string public_key_;
    std::string encryption_private_key_;
    std::string encryption_public_key_;
    std::string storage_key_;
    std::function<void(const Json&)> owner_action_required_;

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
