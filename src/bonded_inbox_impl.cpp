#include "bonded_inbox_impl.h"

#include "domain/state_machine.h"
#include "runtime/skill_registry.h"
#include "services/bond_service.h"
#include "services/inbox_service.h"
#include "storage/database.h"

#include <filesystem>

using bonded::Json;

namespace {

Json ok(Json result = Json::object())
{
    return Json{{"ok", true}, {"result", std::move(result)}};
}

bonded::Submission parseSubmission(const Json& json)
{
    return {json.at("inbox_id").get<std::string>(),
            json.at("message_id").get<std::string>(),
            json.at("idempotency_key").get<std::string>(),
            json.at("sender").get<std::string>(),
            json.at("policy_hash").get<std::string>(),
            json.value("bond_id", ""),
            json.value("bond_amount", std::uint64_t{0}),
            json.at("now_unix").get<std::uint64_t>(),
            json.value("trusted_contact", false),
            json.value("attachment_count", std::uint32_t{0}),
            json.value("attachment_bytes", std::uint64_t{0}),
            json.value("attachment_type", "")};
}

} // namespace

void BondedInboxImpl::onContextReady()
{
    stateChanged(Json{{"type", "runtime.context_ready"}}.dump());
}

std::string BondedInboxImpl::initialize(const std::string& configuration_json)
{
    try {
        const auto configuration = Json::parse(configuration_json);
        profile_name_ = configuration.at("profile").get<std::string>();
        const auto profile = bonded::profileFromString(profile_name_);
        (void)profile;
        data_directory_ = configuration.at("data_directory").get<std::string>();
        if (data_directory_.empty()) {
            throw bonded::DomainError("data_directory is required");
        }
        std::filesystem::create_directories(data_directory_);
        database_ = std::make_unique<bonded::Database>(
            std::filesystem::path(data_directory_) / "bonded-inbox.db");
        database_->migrate();
        bonds_ = std::make_unique<bonded::BondService>();
        inbox_ = std::make_unique<bonded::InboxService>(*database_, *bonds_);
        skills_ = std::make_unique<bonded::SkillRegistry>();
        registerSkills();
        const auto response = ok(Json{{"profile", profile_name_},
                                      {"data_directory", data_directory_},
                                      {"skill_count", skills_->size()}});
        stateChanged(Json{{"type", "runtime.initialized"}, {"profile", profile_name_}}.dump());
        return response.dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::getStatus()
{
    try {
        requireInitialized();
        return ok(Json{{"state", "ready"},
                       {"profile", profile_name_},
                       {"skill_count", skills_->size()},
                       {"bond_count", bonds_->size()}})
            .dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::publishPolicy(const std::string& policy_json)
{
    try {
        requireInitialized();
        auto policy = Json::parse(policy_json).get<bonded::InboxPolicy>();
        policy = inbox_->publishPolicy(policy);
        stateChanged(Json{{"type", "policy.published"},
                          {"inbox_id", policy.inbox_id},
                          {"version", policy.version}}
                         .dump());
        return ok(Json(policy)).dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::submitMessage(const std::string& submission_json)
{
    try {
        requireInitialized();
        const auto message = inbox_->submit(parseSubmission(Json::parse(submission_json)));
        stateChanged(Json{{"type", "message.submitted"},
                          {"message_id", message.id},
                          {"state", bonded::toString(message.state)}}
                         .dump());
        return ok(Json(message)).dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::decideMessage(const std::string& decision_json)
{
    try {
        requireInitialized();
        const auto decision = Json::parse(decision_json);
        const auto state = bonded::messageStateFromString(decision.at("decision").get<std::string>());
        const auto message = inbox_->decide(decision.at("message_id").get<std::string>(), state,
                                            decision.value("explicit_owner_action", false),
                                            decision.value("deterministic_violation", false));
        stateChanged(Json{{"type", "message.settled"},
                          {"message_id", message.id},
                          {"outcome", bonded::toString(*message.settlement)}}
                         .dump());
        return ok(Json(message)).dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::invokeSkill(const std::string& request_json)
{
    try {
        requireInitialized();
        const auto request = Json::parse(request_json);
        const auto output = skills_->invoke(request.at("skill").get<std::string>(),
                                            bonded::profileFromString(profile_name_),
                                            request.value("input", Json::object()));
        return ok(output).dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

void BondedInboxImpl::registerSkills()
{
    const Json object_schema{{"type", "object"}};
    skills_->registerSkill({"meta.skills",
                            "List skills available to this profile",
                            object_schema,
                            Json{{"type", "array"}},
                            {bonded::Profile::Inbox, bonded::Profile::Vault,
                             bonded::Profile::Settlement},
                            [this](const Json&) {
                                return skills_->manifest(bonded::profileFromString(profile_name_));
                            }});
    skills_->registerSkill({"meta.status",
                            "Return redacted runtime status",
                            object_schema,
                            object_schema,
                            {bonded::Profile::Inbox, bonded::Profile::Vault,
                             bonded::Profile::Settlement},
                            [this](const Json&) {
                                return Json{{"state", "ready"}, {"profile", profile_name_}};
                            }});
}

void BondedInboxImpl::requireInitialized() const
{
    if (!database_ || !bonds_ || !inbox_ || !skills_) {
        throw bonded::DomainError("runtime is not initialized");
    }
}

std::string BondedInboxImpl::failure(const std::exception& error) const
{
    return Json{{"ok", false}, {"error", Json{{"code", "request_failed"}, {"message", error.what()}}}}
        .dump();
}

BondedInboxImpl::~BondedInboxImpl() = default;
