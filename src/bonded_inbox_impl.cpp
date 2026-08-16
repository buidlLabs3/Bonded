#include "bonded_inbox_impl.h"

#include "domain/state_machine.h"
#include "runtime/skill_registry.h"
#include "services/bond_service.h"
#include "services/inbox_service.h"
#include "storage/database.h"
#include "security/crypto.h"
#include "logos_sdk.h"
#include "logos_codec.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <fcntl.h>
#include <limits>
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

std::vector<std::string> accountIds(const Json& parameters)
{
    const auto accounts = parameters.at("account_ids").get<std::vector<std::string>>();
    if (accounts.empty()) {
        throw bonded::DomainError("LEZ program call requires account_ids");
    }
    for (const auto& account : accounts) {
        if (account.size() != 64 ||
            !std::all_of(account.begin(), account.end(), [](const auto byte) {
                return std::isxdigit(static_cast<unsigned char>(byte)) != 0;
            })) {
            throw bonded::DomainError("LEZ program call contains an invalid account id");
        }
    }
    return accounts;
}

LogosList instructionWords(const Json& parameters)
{
    const auto& words = parameters.at("instruction_words");
    if (!words.is_array() || words.empty()) {
        throw bonded::DomainError("LEZ program call requires instruction_words");
    }
    for (const auto& word : words) {
        if (!word.is_number_unsigned() || word.get<std::uint64_t>() > UINT32_MAX) {
            throw bonded::DomainError("LEZ instruction word exceeds uint32");
        }
    }
    return words;
}

std::vector<std::uint8_t> programBytes(const Json& value)
{
    static constexpr std::size_t maximum_program_bytes = 32U * 1024U * 1024U;
    const auto bytes = value.get<std::vector<std::uint8_t>>();
    if (bytes.empty() || bytes.size() > maximum_program_bytes) {
        throw bonded::DomainError("LEZ program bytes must be between 1 byte and 32 MiB");
    }
    return bytes;
}

LogosList programDependencies(const Json& parameters)
{
    const auto& values = parameters.value("program_dependencies", Json::array());
    if (!values.is_array() || values.size() > 32) {
        throw bonded::DomainError("LEZ program_dependencies must contain at most 32 entries");
    }
    LogosList encoded = Json::array();
    for (const auto& value : values) {
        encoded.push_back(logos::bytesToJson(programBytes(value)));
    }
    return encoded;
}

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
        auto configuration = Json::parse(configuration_json);
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
        std::unique_ptr<bonded::WalletAdapter> wallet_adapter =
            std::make_unique<UnavailableWalletAdapter>();
        std::string wallet_adapter_name = "official-lez-wallet-not-configured";
        if (!configuration.value("owner_controller", false)) {
            const auto account_id = configuration.at("lez_account_id").get<std::string>();
            const auto account_kind = configuration.value("lez_account_kind", "private-owned");
            if (account_kind != "private-owned") {
                throw bonded::DomainError("live agents require a private-owned LEZ account");
            }
            const auto payment_keys = Json::parse(
                modules().lez_core.get_private_account_keys(account_id));
            if (!payment_keys.is_object() ||
                payment_keys.value("nullifier_public_key", "").size() != 64) {
                throw bonded::DomainError("LEZ Core returned invalid private account keys");
            }
            configuration["lez_private_payment_keys"] = payment_keys;
            wallet_adapter = std::make_unique<bonded::LezWalletAdapter>(
                account_id, false,
                [this](const std::string& account, bool public_account) {
                    return modules().lez_core.get_balance(account, public_account);
                },
                [this](const std::string& from, const std::string& to,
                       const std::string& amount) {
                    return modules().lez_core.transfer_private_owned(from, to, amount);
                },
                [this](const std::string& from, const Json& recipient_keys,
                       const std::string& amount) {
                    return modules().lez_core.transfer_private(
                        from, recipient_keys.dump(), amount);
                },
                [this](const std::string& hash) {
                    return modules().lez_core.poll_transaction_status(hash);
                },
                [database = database.get()] { return database->walletHistory(); },
                [database = database.get()](const bonded::WalletTransfer& transfer) {
                    database->recordWalletTransfer(transfer);
                });
            wallet_adapter_name = "logos-lez-core-0.4.0";
        }
        auto program_adapter = std::make_unique<bonded::LezProgramAdapter>(
            [this](const std::string& account) {
                return modules().lez_core.get_account_public(account);
            },
            [this](const std::string& account) {
                return modules().lez_core.get_account_private(account);
            },
            [this](const std::string& program_id, const Json& parameters) {
                const auto accounts = accountIds(parameters);
                const auto& signing = parameters.at("signing_requirements");
                if (!signing.is_array() || signing.size() != accounts.size() ||
                    !std::all_of(signing.begin(), signing.end(),
                                 [](const auto& item) { return item.is_boolean(); })) {
                    throw bonded::DomainError(
                        "LEZ signing_requirements must match account_ids");
                }
                return modules().lez_core.send_generic_public_transaction(
                    accounts, signing, instructionWords(parameters), program_id);
            },
            [this](const std::string&, const Json& parameters) {
                return modules().lez_core.send_generic_private_transaction(
                    accountIds(parameters), instructionWords(parameters),
                    programBytes(parameters.at("program_elf")),
                    programDependencies(parameters));
            },
            [this](const std::vector<std::uint8_t>& bytes) {
                return modules().lez_core.send_program_deployment_transaction(bytes);
            },
            [this](const std::string& hash) {
                return modules().lez_core.poll_transaction_status(hash);
            });
        bonded::RuntimeAdapters adapters{
            std::move(messaging_adapter), std::move(storage_adapter),
            std::move(wallet_adapter), std::move(program_adapter), "logos-delivery-module",
            "logos-storage-module", std::move(wallet_adapter_name), "logos-lez-core-0.4.0"};
        auto skill_runtime = std::make_unique<bonded::SkillRuntime>(
            *skills, profile, configuration, [this](const Json& proposal) {
                ownerActionRequired(proposal.dump());
            }, std::move(adapters), database.get());
        skill_runtime->registerDefaultSkills();
        profile_name_ = profile_name;
        data_directory_ = data_directory;
        database_ = std::move(database);
        bonds_ = std::move(bonds);
        inbox_ = std::move(inbox);
        skills_ = std::move(skills);
        skill_runtime_ = std::move(skill_runtime);
        skill_runtime_->setOwnerCommandHandler(
            [this](const std::string& action, const Json& payload) {
                return executeOwnerCommand(action, payload);
            });
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

std::string BondedInboxImpl::initializeOwnerController()
{
    try {
        if (skill_runtime_) {
            return ok(Json{{"already_initialized", true},
                           {"status", skill_runtime_->status()}})
                .dump();
        }
        if (!context_ready_ || instancePersistencePath().empty()) {
            throw bonded::DomainError("Basecamp controller persistence is unavailable");
        }
        const auto root = std::filesystem::path(instancePersistencePath()) / "owner-controller";
        std::filesystem::create_directories(root);
        const Json delivery{{"mode", "Edge"}, {"preset", "logos.test"}};
        requireSuccess(modules().delivery_module.createNode(delivery.dump()),
                       "Logos Delivery createNode");
        requireSuccess(modules().delivery_module.start(), "Logos Delivery start");
        const Json storage{{"data-dir", (root / "storage").string()},
                           {"log-level", "INFO"},
                           {"listen-port", 0},
                           {"disc-port", 0},
                           {"nat", "auto"},
                           {"network", "logos.test"},
                           {"storage-quota", 1073741824},
                           {"mix-enabled", false}};
        if (modules().storage_module.init(storage.dump()) != true) {
            throw bonded::DomainError("Logos Storage init did not return true");
        }
        if (modules().storage_module.start() != true) {
            throw bonded::DomainError("Logos Storage start did not return true");
        }
        return initialize(Json{{"profile", "inbox"},
                               {"network", "lez-testnet"},
                               {"owner_controller", true},
                               {"data_directory", (root / "bonded").string()}}
                              .dump());
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

std::string BondedInboxImpl::getOwnerState()
{
    try {
        requireInitialized();
        return ok(ownerStateJson()).dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

Json BondedInboxImpl::ownerStateJson() const
{
    Json messages = Json::array();
    for (const auto& message : database_->messages()) {
        if (message.state == bonded::MessageState::PendingReview) {
            messages.push_back(message);
        }
    }
    auto state = skill_runtime_->ownerState();
    state["messages"] = std::move(messages);
    return state;
}

std::string BondedInboxImpl::getOwnerAgents(const std::string& request_json)
{
    try {
        requireInitialized();
        const auto request = Json::parse(request_json);
        return ok(skill_runtime_->ownerAgents(
                      request.at("now_unix").get<std::uint64_t>()))
            .dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::requestOwnerCommand(const std::string& request_json)
{
    try {
        requireInitialized();
        const auto request = Json::parse(request_json);
        return ok(skill_runtime_->requestOwnerCommand(
                      request.at("target_agent_id").get<std::string>(),
                      request.at("action").get<std::string>(),
                      request.value("payload", Json::object()),
                      request.at("now_unix").get<std::uint64_t>()))
            .dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

std::string BondedInboxImpl::getOwnerResponses()
{
    try {
        requireInitialized();
        return ok(skill_runtime_->ownerResponses()).dump();
    } catch (const std::exception& error) {
        return failure(error);
    }
}

Json BondedInboxImpl::executeOwnerCommand(const std::string& action,
                                          const Json& payload)
{
    if (action == "state.get") {
        return ownerStateJson();
    }
    if (action == "message.decide") {
        const auto state = bonded::messageStateFromString(
            payload.at("decision").get<std::string>());
        const auto message = inbox_->decide(payload.at("message_id").get<std::string>(),
                                            state, true,
                                            payload.value("deterministic_violation", false));
        return message;
    }
    if (action == "spending.decide") {
        return skill_runtime_->decideSpending(
            payload.at("proposal_id").get<std::string>(),
            payload.at("approved").get<bool>(),
            payload.at("now_unix").get<std::uint64_t>());
    }
    if (action == "configuration.update") {
        return skill_runtime_->updateOwnerConfiguration(
            payload.at("changes"),
            payload.at("expected_revision").get<std::uint64_t>());
    }
    throw bonded::DomainError("unsupported owner channel action");
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

std::string BondedInboxImpl::decideSpending(const std::string& decision_json)
{
    try {
        requireInitialized();
        const auto decision = Json::parse(decision_json);
        const auto proposal = skill_runtime_->decideSpending(
            decision.at("proposal_id").get<std::string>(),
            decision.at("approved").get<bool>(),
            decision.at("now_unix").get<std::uint64_t>());
        stateChanged(Json{{"type", "spending.decided"},
                          {"proposal_id", proposal.at("id")},
                          {"state", proposal.at("state")}}
                         .dump());
        return ok(proposal).dump();
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
