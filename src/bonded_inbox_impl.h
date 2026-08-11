#pragma once

#include <exception>
#include <memory>
#include <string>

#include "logos_module_context.h"
#include "runtime/skill_registry.h"
#include "services/bond_service.h"
#include "services/inbox_service.h"
#include "storage/database.h"

class BondedInboxImpl : public LogosModuleContext {
public:
    ~BondedInboxImpl();
    std::string initialize(const std::string& configuration_json);
    std::string getStatus();
    std::string publishPolicy(const std::string& policy_json);
    std::string submitMessage(const std::string& submission_json);
    std::string decideMessage(const std::string& decision_json);
    std::string invokeSkill(const std::string& request_json);
    void onContextReady() override;

logos_events:
    void stateChanged(const std::string& event_json);
    void ownerActionRequired(const std::string& proposal_json);

private:
    void requireInitialized() const;
    void registerSkills();
    std::string failure(const std::exception& error) const;

    std::string data_directory_;
    std::string profile_name_;
    std::unique_ptr<bonded::Database> database_;
    std::unique_ptr<bonded::BondService> bonds_;
    std::unique_ptr<bonded::InboxService> inbox_;
    std::unique_ptr<bonded::SkillRegistry> skills_;
};
