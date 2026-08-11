#pragma once

#include "integrations/memory_adapters.h"
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

class SkillRuntime {
public:
    SkillRuntime(SkillRegistry& registry, Profile profile, const Json& configuration,
                 std::function<void(const Json&)> owner_action_required);

    void registerDefaultSkills();
    Json status() const;

private:
    Json handler(const std::string& name, const Json& input);
    AgentCard ownCard(std::uint64_t now_unix, std::uint64_t expires_at,
                      std::uint64_t task_price) const;
    static Json spendingProposalJson(const SpendingProposal& proposal);

    SkillRegistry& registry_;
    Profile profile_;
    std::string network_;
    std::string agent_id_;
    std::string private_key_;
    std::string public_key_;
    std::string storage_key_;
    std::function<void(const Json&)> owner_action_required_;

    MemoryMessagingAdapter messaging_adapter_;
    MemoryStorageAdapter storage_adapter_;
    MemoryWalletAdapter wallet_adapter_;
    MemoryProgramAdapter program_adapter_;
    MessagingService messaging_;
    StorageService storage_;
    SpendingController spending_;
    A2AService a2a_;
    ConfigurationService configuration_;
};

} // namespace bonded
