#include "bonded_inbox_impl.h"

#include "domain/state_machine.h"
#include "runtime/skill_registry.h"
#include "services/bond_service.h"
#include "services/inbox_service.h"
#include "storage/database.h"
#include "security/crypto.h"
#include "logos_sdk.h"

#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

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

std::string resultString(const StdLogosResult& result, const std::string& operation)
{
    if (!result.success) {
        throw bonded::DomainError(operation + " failed: " + result.error);
    }
    if (!result.value.is_string()) {
        throw bonded::DomainError(operation + " returned a non-string result");
    }
    const auto value = result.value.get<std::string>();
    if (value.empty()) {
        throw bonded::DomainError(operation + " returned an empty result");
    }
    return value;
}

void requireSuccess(const StdLogosResult& result, const std::string& operation)
{
    if (!result.success) {
        throw bonded::DomainError(operation + " failed: " + result.error);
    }
}

void writeOwnerOnly(const std::filesystem::path& path, const std::string& payload)
{
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw bonded::DomainError("could not create encrypted Storage staging file");
    }
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto written = ::write(descriptor, payload.data() + offset, payload.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ::close(descriptor);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw bonded::DomainError("could not write encrypted Storage staging file");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::close(descriptor) != 0) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw bonded::DomainError("could not close encrypted Storage staging file");
    }
}

class UnavailableWalletAdapter final : public bonded::WalletAdapter {
public:
    std::uint64_t balance() const override
    {
        throw bonded::DomainError("official LEZ wallet host API is unavailable");
    }
    std::string send(const std::string&, std::uint64_t, std::uint64_t) override
    {
        throw bonded::DomainError("official LEZ wallet host API is unavailable");
    }
    std::vector<bonded::WalletTransfer> history() const override
    {
        throw bonded::DomainError("official LEZ wallet host API is unavailable");
    }
};

class UnavailableProgramAdapter final : public bonded::ProgramAdapter {
public:
    bonded::Json query(const std::string&, const bonded::Json&) const override
    {
        throw bonded::DomainError("official LEZ program host API is unavailable");
    }
    std::string call(const std::string&, const std::string&, const bonded::Json&) override
    {
        throw bonded::DomainError("official LEZ program host API is unavailable");
    }
    std::string deploy(const std::string&) override
    {
        throw bonded::DomainError("official LEZ program host API is unavailable");
    }
};

} // namespace

void BondedInboxImpl::onContextReady()
{
    if (context_ready_) {
        return;
    }
    modules().delivery_module.onMessageReceived(
        [this](const std::string&, const std::string& topic,
               const std::vector<std::uint8_t>& payload, std::int64_t) {
            std::lock_guard lock(adapter_events_mutex_);
            if (active_logos_messaging_) {
                try {
                    active_logos_messaging_->receive(
                        topic, std::string(payload.begin(), payload.end()));
                } catch (...) {
                    stateChanged(Json{{"type", "dependency.event_handler_failed"},
                                      {"dependency", "delivery_module"},
                                      {"event", "message_received"}}
                                     .dump());
                }
            }
        });
    modules().storage_module.onStorageUploadDone([this](const std::string& payload) {
        std::lock_guard lock(adapter_events_mutex_);
        if (active_logos_storage_) {
            try {
                active_logos_storage_->uploadDone(payload);
            } catch (...) {
                stateChanged(Json{{"type", "dependency.invalid_event"},
                                  {"dependency", "storage_module"},
                                  {"event", "upload_done"}}
                                 .dump());
            }
        }
    });
    modules().storage_module.onStorageDownloadProgress([this](const std::string& payload) {
        std::lock_guard lock(adapter_events_mutex_);
        if (active_logos_storage_) {
            try {
                active_logos_storage_->downloadProgress(payload);
            } catch (...) {
                stateChanged(Json{{"type", "dependency.invalid_event"},
                                  {"dependency", "storage_module"},
                                  {"event", "download_progress"}}
                                 .dump());
            }
        }
    });
    modules().storage_module.onStorageDownloadDone([this](const std::string& payload) {
        std::lock_guard lock(adapter_events_mutex_);
        if (active_logos_storage_) {
            try {
                active_logos_storage_->downloadDone(payload);
            } catch (...) {
                stateChanged(Json{{"type", "dependency.invalid_event"},
                                  {"dependency", "storage_module"},
                                  {"event", "download_done"}}
                                 .dump());
            }
        }
    });
    context_ready_ = true;
    stateChanged(Json{{"type", "runtime.context_ready"}}.dump());
}

std::string BondedInboxImpl::initialize(const std::string& configuration_json)
{
    try {
        const auto configuration = Json::parse(configuration_json);
        if (skill_runtime_) {
            throw bonded::DomainError("runtime is already initialized");
        }
        const auto profile_name = configuration.at("profile").get<std::string>();
        const auto profile = bonded::profileFromString(profile_name);
        if (!context_ready_) {
            throw bonded::DomainError("Logos module dependencies are not wired");
        }
        const auto data_directory = configuration.at("data_directory").get<std::string>();
        if (data_directory.empty()) {
            throw bonded::DomainError("data_directory is required");
        }
        std::filesystem::create_directories(data_directory);
        auto database = std::make_unique<bonded::Database>(
            std::filesystem::path(data_directory) / "bonded-inbox.db");
        database->migrate();
        auto bonds = std::make_unique<bonded::BondService>(*database);
        auto inbox = std::make_unique<bonded::InboxService>(*database, *bonds);
        auto skills = std::make_unique<bonded::SkillRegistry>();
        auto messaging_adapter = std::make_unique<bonded::LogosMessagingAdapter>(
            [this](const std::string& topic, const std::string& payload) {
                const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
                return resultString(modules().delivery_module.send(topic, bytes),
                                    "Logos Delivery send");
            },
            [this](const std::string& topic) {
                requireSuccess(modules().delivery_module.subscribe(topic),
                               "Logos Delivery subscribe");
            });
        auto storage_adapter = std::make_unique<bonded::LogosStorageAdapter>(
            [this](const std::string& payload) { return startStorageUpload(payload); },
            [this](const std::string& cid) {
                return resultString(modules().storage_module.downloadChunks(cid, false, 65536),
                                    "Logos Storage download");
            },
            [this](const std::string& session) { cancelStorageUpload(session); },
            [this](const std::string& session) {
                static_cast<void>(modules().storage_module.downloadCancel(session));
            },
            [this](const std::string& session) { cleanupStorageUpload(session); },
            std::chrono::seconds(120));
        auto* const messaging = messaging_adapter.get();
        auto* const storage = storage_adapter.get();
        bonded::RuntimeAdapters adapters{
            std::move(messaging_adapter), std::move(storage_adapter),
            std::make_unique<UnavailableWalletAdapter>(),
            std::make_unique<UnavailableProgramAdapter>(), "logos-delivery-module",
            "logos-storage-module", "official-lez-wallet-host-api-unavailable",
            "official-lez-program-host-api-unavailable"};
        auto skill_runtime = std::make_unique<bonded::SkillRuntime>(
            *skills, profile, configuration, [this](const Json& proposal) {
                ownerActionRequired(proposal.dump());
            }, std::move(adapters));
        skill_runtime->registerDefaultSkills();
        profile_name_ = profile_name;
        data_directory_ = data_directory;
        database_ = std::move(database);
        bonds_ = std::move(bonds);
        inbox_ = std::move(inbox);
        skills_ = std::move(skills);
        skill_runtime_ = std::move(skill_runtime);
        {
            std::lock_guard lock(adapter_events_mutex_);
            active_logos_messaging_ = messaging;
            active_logos_storage_ = storage;
        }
        const auto response = ok(Json{{"profile", profile_name},
                                      {"data_directory", data_directory},
                                      {"skill_count", skills_->size()}});
        stateChanged(Json{{"type", "runtime.initialized"}, {"profile", profile_name_}}.dump());
        return response.dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::startStorageUpload(const std::string& payload)
{
    if (data_directory_.empty()) {
        throw bonded::DomainError("runtime data directory is unavailable");
    }
    const auto path = std::filesystem::path(data_directory_) /
                      (".bonded-upload-" + bonded::Crypto::randomHex(16));
    writeOwnerOnly(path, payload);
    try {
        const auto session = resultString(
            modules().storage_module.uploadUrl(path.string(), 65536), "Logos Storage upload");
        std::filesystem::path duplicate_path;
        {
            std::lock_guard lock(storage_uploads_mutex_);
            const auto [found, inserted] = storage_uploads_.emplace(session, path);
            if (!inserted) {
                duplicate_path = found->second;
                storage_uploads_.erase(found);
            }
        }
        if (!duplicate_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(duplicate_path, ignored);
            static_cast<void>(modules().storage_module.uploadCancel(session));
            throw bonded::DomainError("Logos Storage returned a duplicate upload session");
        }
        return session;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
}

void BondedInboxImpl::cancelStorageUpload(const std::string& session)
{
    static_cast<void>(modules().storage_module.uploadCancel(session));
    cleanupStorageUpload(session);
}

void BondedInboxImpl::cleanupStorageUpload(const std::string& session)
{
    std::filesystem::path path;
    {
        std::lock_guard lock(storage_uploads_mutex_);
        const auto found = storage_uploads_.find(session);
        if (found == storage_uploads_.end()) {
            return;
        }
        path = found->second;
        storage_uploads_.erase(found);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

std::string BondedInboxImpl::getStatus()
{
    try {
        requireInitialized();
        const auto runtime_status = skill_runtime_->status();
        return ok(Json{{"state", runtime_status.at("state")},
                       {"profile", profile_name_},
                       {"skill_count", skills_->size()},
                       {"runtime", runtime_status},
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
    skill_runtime_->registerDefaultSkills();
}

void BondedInboxImpl::requireInitialized() const
{
    if (!database_ || !bonds_ || !inbox_ || !skills_ || !skill_runtime_) {
        throw bonded::DomainError("runtime is not initialized");
    }
}

std::string BondedInboxImpl::failure(const std::exception& error) const
{
    return Json{{"ok", false}, {"error", Json{{"code", "request_failed"}, {"message", error.what()}}}}
        .dump();
}

BondedInboxImpl::~BondedInboxImpl()
{
    {
        std::lock_guard lock(adapter_events_mutex_);
        active_logos_messaging_ = nullptr;
        active_logos_storage_ = nullptr;
        skill_runtime_.reset();
    }
    std::lock_guard lock(storage_uploads_mutex_);
    for (const auto& [_, path] : storage_uploads_) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}
