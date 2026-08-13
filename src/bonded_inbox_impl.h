#pragma once

#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "logos_module_context.h"
#include "runtime/skill_registry.h"
#include "runtime/skill_runtime.h"
#include "services/bond_service.h"
#include "services/inbox_service.h"
#include "integrations/logos_adapters.h"
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
    std::string startStorageUpload(const std::string& payload);
    void cancelStorageUpload(const std::string& session);
    void cleanupStorageUpload(const std::string& session);
    std::string failure(const std::exception& error) const;

    std::string data_directory_;
    std::string profile_name_;
    std::unique_ptr<bonded::Database> database_;
    std::unique_ptr<bonded::BondService> bonds_;
    std::unique_ptr<bonded::InboxService> inbox_;
    std::unique_ptr<bonded::SkillRegistry> skills_;
    std::unique_ptr<bonded::SkillRuntime> skill_runtime_;
    bonded::LogosMessagingAdapter* active_logos_messaging_{nullptr};
    bonded::LogosStorageAdapter* active_logos_storage_{nullptr};
    std::recursive_mutex adapter_events_mutex_;
    std::mutex storage_uploads_mutex_;
    std::map<std::string, std::filesystem::path> storage_uploads_;
    bool context_ready_{false};
};
