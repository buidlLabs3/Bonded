#include "runtime/skill_runtime.h"

#include "runtime/default_skill_catalog.h"
#include "integrations/memory_adapters.h"
#include "security/crypto.h"
#include "storage/database.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace bonded {
namespace {

std::vector<std::string> profileNames(const SkillRegistry& registry, Profile profile)
{
    std::vector<std::string> names;
    for (const auto& item : registry.manifest(profile)) {
        names.push_back(item.at("name").get<std::string>());
    }
    return names;
}

std::vector<std::string> members(const Json& input)
{
    return input.at("members").get<std::vector<std::string>>();
}

std::vector<SpendingProposal> loadSpendingProposals(Database* database)
{
    std::vector<SpendingProposal> proposals;
    if (database == nullptr) {
        return proposals;
    }
    for (const auto& record : database->runtimeRecords("spending-proposal")) {
        proposals.push_back(record.get<SpendingProposal>());
    }
    return proposals;
}

std::vector<AgentCard> loadAgentCards(Database* database)
{
    std::vector<AgentCard> cards;
    if (database != nullptr) {
        for (const auto& record : database->runtimeRecords("a2a-card")) {
            cards.push_back(record.get<AgentCard>());
        }
    }
    return cards;
}

std::vector<A2ATask> loadA2ATasks(Database* database)
{
    std::vector<A2ATask> tasks;
    if (database != nullptr) {
        for (const auto& record : database->runtimeRecords("a2a-task")) {
            tasks.push_back(record.get<A2ATask>());
        }
    }
    return tasks;
}

std::uint64_t unixNow()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t ownerMessageExpiry(std::uint64_t now_unix)
{
    constexpr auto lifetime = std::uint64_t{300};
    if (now_unix > std::numeric_limits<std::uint64_t>::max() - lifetime) {
        throw DomainError("owner channel timestamp overflow");
    }
    return now_unix + lifetime;
}

std::optional<Json> loadRuntimeConfiguration(Database* database)
{
    if (database == nullptr) {
        return std::nullopt;
    }
    const auto records = database->runtimeRecords("configuration");
    if (records.size() > 1) {
        throw DomainError("multiple persisted runtime configurations exist");
    }
    return records.empty() ? std::nullopt : std::optional<Json>{records.front()};
}

template <typename Adapter>
Adapter& requireAdapter(const std::unique_ptr<Adapter>& adapter, const char* name)
{
    if (!adapter) {
        throw DomainError(std::string(name) + " runtime adapter is required");
    }
    return *adapter;
}

struct RuntimeIdentity {
    std::string private_key;
    std::string public_key;
    std::string encryption_private_key;
    std::string encryption_public_key;
    std::string storage_key;
};

void requireKey(const std::string& key, const char* name)
{
    if (key.size() != 64 || Crypto::hexDecode(key).size() != 32) {
        throw DomainError(std::string("runtime identity contains an invalid ") + name);
    }
}

RuntimeIdentity parseIdentity(const Json& value)
{
    RuntimeIdentity identity{value.at("signing_private_key").get<std::string>(),
                             value.at("signing_public_key").get<std::string>(),
                             value.at("encryption_private_key").get<std::string>(),
                             value.at("encryption_public_key").get<std::string>(),
                             value.at("storage_key").get<std::string>()};
    requireKey(identity.private_key, "signing private key");
    requireKey(identity.public_key, "signing public key");
    requireKey(identity.encryption_private_key, "encryption private key");
    requireKey(identity.encryption_public_key, "encryption public key");
    requireKey(identity.storage_key, "storage key");
    const std::string challenge = "bonded-inbox/identity-check/v1";
    if (!Crypto::verifyEd25519(identity.public_key, challenge,
                               Crypto::signEd25519(identity.private_key, challenge))) {
        throw DomainError("runtime identity signing keys do not match");
    }
    return identity;
}

RuntimeIdentity readIdentity(const std::filesystem::path& path)
{
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        throw DomainError("runtime identity must be a regular file");
    }
    if ((status.st_mode & 0077) != 0) {
        throw DomainError("runtime identity permissions must be 0600");
    }
    std::ifstream input(path);
    if (!input) {
        throw DomainError("cannot read runtime identity");
    }
    Json value;
    input >> value;
    if (value.value("schema_version", 0) != 1) {
        throw DomainError("runtime identity has an unsupported schema");
    }
    return parseIdentity(value);
}

RuntimeIdentity createIdentity(const std::filesystem::path& path)
{
    const auto signing = Crypto::generateEd25519KeyPair();
    const auto encryption = Crypto::generateX25519KeyPair();
    RuntimeIdentity identity{signing.first, signing.second, encryption.first,
                             encryption.second, Crypto::randomHex(32)};
    const Json value{{"schema_version", 1},
                     {"signing_private_key", identity.private_key},
                     {"signing_public_key", identity.public_key},
                     {"encryption_private_key", identity.encryption_private_key},
                     {"encryption_public_key", identity.encryption_public_key},
                     {"storage_key", identity.storage_key}};
    const auto payload = value.dump(2) + "\n";
    const auto descriptor =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return readIdentity(path);
        }
        throw DomainError("cannot create runtime identity");
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
            throw DomainError("cannot write runtime identity");
        }
        offset += static_cast<std::size_t>(written);
    }
    const auto sync_result = ::fsync(descriptor);
    const auto close_result = ::close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw DomainError("cannot persist runtime identity");
    }
    return identity;
}

RuntimeIdentity runtimeIdentity(const Json& configuration)
{
    if (!configuration.contains("data_directory")) {
        const auto signing = Crypto::generateEd25519KeyPair();
        const auto encryption = Crypto::generateX25519KeyPair();
        return {signing.first, signing.second, encryption.first, encryption.second,
                Crypto::randomHex(32)};
    }
    const auto directory =
        std::filesystem::path(configuration.at("data_directory").get<std::string>());
    if (directory.empty()) {
        throw DomainError("runtime identity data_directory is empty");
    }
    std::filesystem::create_directories(directory);
    std::filesystem::permissions(
        directory, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    const auto path = directory / "identity.json";
    return std::filesystem::exists(path) ? readIdentity(path) : createIdentity(path);
}

} // namespace

RuntimeAdapters RuntimeAdapters::memory(std::uint64_t initial_balance)
{
    return {std::make_unique<MemoryMessagingAdapter>(),
            std::make_unique<MemoryStorageAdapter>(),
            std::make_unique<MemoryWalletAdapter>(initial_balance),
            std::make_unique<MemoryProgramAdapter>(),
            "memory-test-double",
            "memory-test-double",
            "memory-test-double",
            "memory-test-double"};
}

SkillRuntime::SkillRuntime(SkillRegistry& registry, Profile profile, const Json& configuration,
                           std::function<void(const Json&)> owner_action_required)
    : SkillRuntime(registry, profile, configuration, std::move(owner_action_required),
                   RuntimeAdapters::memory(
                       configuration.value("initial_balance", std::uint64_t{1000})))
{
}

SkillRuntime::SkillRuntime(SkillRegistry& registry, Profile profile, const Json& configuration,
                           std::function<void(const Json&)> owner_action_required,
                           RuntimeAdapters adapters, Database* database)
    : registry_(registry), profile_(profile),
      owner_controller_(configuration.value("owner_controller", false)),
      network_(configuration.value("network", "logos-local")),
      owner_public_key_(configuration.value("owner_public_key", "")),
      owner_action_required_(std::move(owner_action_required)),
      database_(database),
      messaging_adapter_(std::move(adapters.messaging)),
      storage_adapter_(std::move(adapters.storage)),
      wallet_adapter_(std::move(adapters.wallet)),
      program_adapter_(std::move(adapters.program)),
      messaging_adapter_name_(std::move(adapters.messaging_name)),
      storage_adapter_name_(std::move(adapters.storage_name)),
      wallet_adapter_name_(std::move(adapters.wallet_name)),
      program_adapter_name_(std::move(adapters.program_name)),
      messaging_(requireAdapter(messaging_adapter_, "messaging"), network_),
      storage_(requireAdapter(storage_adapter_, "storage")),
      spending_(requireAdapter(wallet_adapter_, "wallet"),
                SpendingPolicy{configuration.value("per_transaction_limit", std::uint64_t{100}),
                               configuration.value("per_period_limit", std::uint64_t{500}),
                               configuration.value("period_seconds", std::uint64_t{86400}),
                               configuration.value("approval_timeout_seconds",
                                                   std::uint64_t{3600})},
                [database] { return loadSpendingProposals(database); },
                [database](const SpendingProposal& proposal) {
                    if (database != nullptr) {
                        database->upsertRuntimeRecord("spending-proposal", proposal.id,
                                                      Json(proposal));
                    }
                }),
      a2a_(requireAdapter(messaging_adapter_, "messaging"), network_,
           [database] { return loadAgentCards(database); },
           [database](const AgentCard& card) {
               if (database != nullptr) {
                   database->upsertRuntimeRecord("a2a-card", card.agent_id, Json(card));
               }
           },
           [database] { return loadA2ATasks(database); },
           [database](const A2ATask& task) {
               if (database != nullptr) {
                   database->upsertRuntimeRecord("a2a-task", task.id, Json(task));
               }
           },
           [this](const std::string& recipient, std::uint64_t amount,
                  std::uint64_t now_unix, const std::string& request_id) {
               const auto proposal =
                   spending_.propose(recipient, amount, now_unix, request_id);
               const auto output = spendingProposalJson(proposal);
               if (proposal.state == ApprovalState::Pending && owner_action_required_) {
                   owner_action_required_(Json{{"type", "spending.approval_required"},
                                               {"proposal", output}});
               }
               return output;
           }),
      configuration_(Json{{"classifier_enabled", configuration.value("classifier_enabled", true)},
                          {"rate_limit", configuration.value("rate_limit", std::uint64_t{10})},
                          {"owner_notifications",
                           configuration.value("owner_notifications", true)},
                          {"approval_timeout_seconds",
                           configuration.value("approval_timeout_seconds",
                                               std::uint64_t{3600})}},
                     configuration.value("owner_public_key", ""),
                     [database] { return loadRuntimeConfiguration(database); },
                     [database](const Json& state) {
                         if (database != nullptr) {
                             database->upsertRuntimeRecord("configuration", "current", state);
                         }
                     })
{
    const auto identity = runtimeIdentity(configuration);
    private_key_ = identity.private_key;
    public_key_ = identity.public_key;
    encryption_private_key_ = identity.encryption_private_key;
    encryption_public_key_ = identity.encryption_public_key;
    storage_key_ = identity.storage_key;
    agent_id_ = "npk:" + Crypto::sha256(public_key_).substr(0, 32);
    a2a_.configureTransport(agent_id_, private_key_, public_key_, encryption_private_key_);

    if (!owner_public_key_.empty()) {
        requireKey(owner_public_key_, "owner signing public key");
        messaging_adapter_->subscribe(ownerRequestTopic(agent_id_), [this](const std::string& raw) {
            receiveOwnerRequest(raw);
        });
    }
    if (database_ != nullptr) {
        for (const auto& record : database_->runtimeRecords("owner-response")) {
            try {
                owner_responses_.emplace(record.at("requestId").get<std::string>(), record);
            } catch (const std::exception&) {
                continue;
            }
        }
        while (owner_responses_.size() > 1024) {
            const auto expired = owner_responses_.begin()->first;
            owner_responses_.erase(owner_responses_.begin());
            database_->deleteRuntimeRecord("owner-response", expired);
        }
        for (const auto& record : database_->runtimeRecords("owner-pending")) {
            try {
                const auto request_id = record.at("requestId").get<std::string>();
                if (!owner_responses_.contains(request_id)) {
                    owner_pending_.emplace(request_id, record.at("card").get<AgentCard>());
                }
            } catch (const std::exception&) {
                continue;
            }
        }
        for (const auto& record : database_->runtimeRecords("owner-processed")) {
            try {
                owner_processed_.insert(record.at("envelopeId").get<std::string>());
            } catch (const std::exception&) {
                continue;
            }
        }
        while (owner_processed_.size() > 1024) {
            const auto expired = *owner_processed_.begin();
            owner_processed_.erase(owner_processed_.begin());
            database_->deleteRuntimeRecord("owner-processed", expired);
        }
    }
    messaging_adapter_->subscribe(ownerResponseTopic(agent_id_), [this](const std::string& raw) {
        receiveOwnerResponse(raw);
    });
}

void SkillRuntime::registerDefaultSkills()
{
    const Json object_schema{{"type", "object"}};
    for (const auto& spec : allDefaultSkillSpecs()) {
        registry_.registerSkill(
            SkillDefinition{spec.name, spec.description, object_schema, object_schema, spec.profiles,
                            [this, name = spec.name](const Json& input) {
                                return handler(name, input);
                            }});
    }
}

Json SkillRuntime::spendingProposalJson(const SpendingProposal& proposal)
{
    return Json{{"id", proposal.id},
                {"recipient", proposal.recipient},
                {"amount", proposal.amount},
                {"created_at", proposal.created_at},
                {"expires_at", proposal.expires_at},
                {"state", toString(proposal.state)},
                {"transfer_id", proposal.transfer_id}};
}

AgentCard SkillRuntime::ownCard(std::uint64_t now_unix, std::uint64_t expires_at,
                                std::uint64_t task_price,
                                const std::string& payment_recipient) const
{
    if (expires_at <= now_unix) {
        throw DomainError("Agent Card expiry must be in the future");
    }
    if (task_price > 0 &&
        (payment_recipient.size() != 64 || Crypto::hexDecode(payment_recipient).size() != 32)) {
        throw DomainError("paid Agent Card requires a 32-byte hex payment recipient");
    }
    AgentCard card{"a2a/1.0",
                   network_,
                   agent_id_,
                   public_key_,
                   profileNames(registry_, profile_),
                   Json{{"streaming", true},
                        {"paid_tasks", true},
                        {"messaging_encryption", "x25519-aes-256-gcm"},
                        {"messaging_encryption_public_key", encryption_public_key_},
                        {"payment_recipient", payment_recipient}},
                   "/bonded-inbox/1/a2a-task/json",
                   task_price,
                   expires_at,
                   ""};
    return A2AService::signCard(std::move(card), private_key_);
}

Json SkillRuntime::handler(const std::string& name, const Json& input)
{
    if (name == "storage.upload") {
        return storage_.upload(input.at("plaintext").get<std::string>(),
                               input.at("label").get<std::string>(), storage_key_);
    }
    if (name == "storage.download") {
        return Json{{"plaintext",
                     storage_.download(input.at("address").get<std::string>(), storage_key_)}};
    }
    if (name == "storage.list") {
        return storage_.list();
    }
    if (name == "storage.share") {
        return storage_.share(input.at("address").get<std::string>(),
                              input.at("recipient").get<std::string>(),
                              input.at("expires_at").get<std::uint64_t>(),
                              input.at("wrapped_key").get<std::string>());
    }
    if (name == "messaging.send") {
        const auto now = input.at("now_unix").get<std::uint64_t>();
        SignedEnvelope envelope{"bonded-inbox/envelope/v2",
                                network_,
                                input.value("id", Crypto::randomHex(16)),
                                agent_id_,
                                input.at("recipient").get<std::string>(),
                                input.at("topic").get<std::string>(),
                                Crypto::randomHex(16),
                                input.value("expires_at", now + 300),
                                "",
                                "",
                                "",
                                "",
                                "",
                                ""};
        envelope = MessagingService::sealAndSign(
            std::move(envelope), input.at("payload").get<std::string>(),
            input.at("recipient_encryption_public_key").get<std::string>(), private_key_,
            public_key_);
        return Json{{"message_id", messaging_.send(envelope, now)}};
    }
    if (name == "messaging.join") {
        messaging_adapter_->join(input.at("group_id").get<std::string>());
        return Json{{"joined", true}};
    }
    if (name == "messaging.create_group") {
        return Json{{"group_id", messaging_adapter_->createGroup(members(input))}};
    }
    if (name == "wallet.balance") {
        return Json{{"balance", wallet_adapter_->balance()}, {"asset", "LEZ"}};
    }
    if (name == "wallet.send") {
        const auto proposal = spending_.propose(input.at("recipient").get<std::string>(),
                                                input.at("amount").get<std::uint64_t>(),
                                                input.at("now_unix").get<std::uint64_t>(),
                                                input.at("request_id").get<std::string>());
        const auto output = spendingProposalJson(proposal);
        if (proposal.state == ApprovalState::Pending && owner_action_required_) {
            owner_action_required_(Json{{"type", "spending.approval_required"},
                                        {"proposal", output}});
        }
        return output;
    }
    if (name == "wallet.history") {
        Json history = Json::array();
        for (const auto& transfer : wallet_adapter_->history()) {
            history.push_back(Json{{"id", transfer.id},
                                   {"recipient", transfer.recipient},
                                   {"amount", transfer.amount},
                                   {"timestamp_unix", transfer.timestamp_unix}});
        }
        return history;
    }
    if (name == "program.query") {
        return program_adapter_->query(input.at("program_id").get<std::string>(),
                                      input.value("parameters", Json::object()));
    }
    if (name == "program.call") {
        return Json{{"call_id", program_adapter_->call(
                                    input.at("program_id").get<std::string>(),
                                    input.at("instruction").get<std::string>(),
                                    input.value("parameters", Json::object()))}};
    }
    if (name == "program.deploy") {
        return Json{{"program_id",
                     program_adapter_->deploy(input.at("binary_path").get<std::string>())}};
    }
    if (name == "agent.card") {
        const auto now = input.value("now_unix", std::uint64_t{0});
        if (input.contains("card")) {
            return a2a_.publishCard(input.at("card").get<AgentCard>(), now);
        }
        const auto card = ownCard(now, input.at("expires_at").get<std::uint64_t>(),
                                  input.value("task_price", std::uint64_t{0}),
                                  input.value("payment_recipient", ""));
        if (input.value("publish", true)) {
            a2a_.publishCard(card, now);
        }
        return card;
    }
    if (name == "agent.discover") {
        return a2a_.discover(input.value("skill", ""),
                             input.at("now_unix").get<std::uint64_t>());
    }
    if (name == "agent.task") {
        const auto action = input.value("action", "create");
        if (action == "complete") {
            return a2a_.complete(input.at("task_id").get<std::string>(),
                                 input.value("output", Json::object()),
                                 input.at("now_unix").get<std::uint64_t>());
        }
        if (action == "fail") {
            return a2a_.fail(input.at("task_id").get<std::string>(),
                             input.at("reason").get<std::string>(),
                             input.at("now_unix").get<std::uint64_t>());
        }
        if (action == "input_required") {
            return a2a_.requireInput(input.at("task_id").get<std::string>(),
                                     input.at("now_unix").get<std::uint64_t>());
        }
        A2ATask task{input.at("task_id").get<std::string>(),
                     input.value("requester", agent_id_),
                     input.at("provider").get<std::string>(),
                     input.at("skill").get<std::string>(),
                     input.value("input", Json::object()),
                     Json::object(),
                     input.value("price", std::uint64_t{0}),
                     input.at("expires_at").get<std::uint64_t>(),
                     A2ATaskState::Working,
                     "",
                     0};
        return a2a_.createTask(std::move(task), input.at("now_unix").get<std::uint64_t>());
    }
    if (name == "agent.subscribe") {
        return a2a_.subscribe(input.at("task_id").get<std::string>());
    }
    if (name == "agent.cancel") {
        return a2a_.cancel(input.at("task_id").get<std::string>(),
                           input.at("now_unix").get<std::uint64_t>());
    }
    if (name == "meta.skills") {
        return registry_.manifest(profile_);
    }
    if (name == "meta.status") {
        return status();
    }
    if (name == "meta.configure") {
        return configuration_.update(input);
    }
    throw DomainError("default skill handler is missing: " + name);
}

Json SkillRuntime::status() const
{
    std::uint64_t storage_bytes = 0;
    for (const auto& entry : storage_.list()) {
        storage_bytes += entry.plaintext_bytes;
    }
    Json balance = nullptr;
    std::string wallet_error;
    const auto wallet_required = !owner_controller_;
    if (wallet_required) {
        try {
            balance = wallet_adapter_->balance();
        } catch (const std::exception& error) {
            wallet_error = error.what();
        }
    }
    const auto program_unavailable =
        program_adapter_name_ == "official-lez-program-host-api-unavailable";
    const auto program_required = profile_ != Profile::Inbox;
    const auto degraded = (wallet_required && !wallet_error.empty()) ||
                          (program_required && program_unavailable);
    return Json{{"state", degraded ? "degraded" : "ready"},
                {"profile", toString(profile_)},
                {"agent_id", agent_id_},
                {"signing_public_key", public_key_},
                {"messaging_encryption_public_key", encryption_public_key_},
                {"balance", balance},
                {"wallet_error", wallet_error},
                {"program_error",
                 program_unavailable ? "official LEZ program host API is unavailable" : ""},
                {"storage_bytes", storage_bytes},
                {"active_tasks", a2a_.activeTaskCount()},
                {"configuration", configuration_.snapshot()},
                {"dependencies", Json{{"messaging", messaging_adapter_name_},
                                       {"storage", storage_adapter_name_},
                                       {"wallet", wallet_adapter_name_},
                                       {"program", program_adapter_name_}}}};
}

Json SkillRuntime::ownerState() const
{
    Json approvals = Json::array();
    for (const auto& proposal : spending_.list()) {
        if (proposal.state == ApprovalState::Pending) {
            approvals.push_back(spendingProposalJson(proposal));
        }
    }
    return Json{{"runtime", status()},
                {"approvals", std::move(approvals)},
                {"tasks", a2a_.tasks()}};
}

Json SkillRuntime::decideSpending(const std::string& proposal_id, bool approved,
                                  std::uint64_t now_unix)
{
    const auto proposal = approved ? spending_.approve(proposal_id, now_unix)
                                   : spending_.deny(proposal_id);
    if (proposal.state == ApprovalState::Executed) {
        a2a_.recordSettlement(proposal.id, proposal.transfer_id, now_unix);
    }
    return spendingProposalJson(proposal);
}

Json SkillRuntime::updateOwnerConfiguration(const Json& changes,
                                            std::uint64_t expected_revision)
{
    return configuration_.updateAuthenticated(changes, expected_revision);
}

std::string SkillRuntime::ownerRequestTopic(const std::string& agent_id)
{
    return "/bonded-inbox/1/owner/" + Crypto::sha256(agent_id).substr(0, 32) + "/request";
}

std::string SkillRuntime::ownerResponseTopic(const std::string& agent_id)
{
    return "/bonded-inbox/1/owner/" + Crypto::sha256(agent_id).substr(0, 32) + "/response";
}

void SkillRuntime::setOwnerCommandHandler(OwnerCommand handler)
{
    if (!handler) {
        throw DomainError("owner command handler is required");
    }
    std::lock_guard lock(owner_mutex_);
    if (owner_command_) {
        throw DomainError("owner command handler is already configured");
    }
    owner_command_ = std::move(handler);
}

Json SkillRuntime::ownerAgents(std::uint64_t now_unix) const
{
    Json agents = Json::array();
    for (const auto& card : a2a_.discover("", now_unix)) {
        if (card.agent_id != agent_id_) agents.push_back(card);
    }
    return agents;
}

Json SkillRuntime::requestOwnerCommand(const std::string& target_agent_id,
                                       const std::string& action, const Json& payload,
                                       std::uint64_t now_unix)
{
    if (target_agent_id.empty() || action.empty() || !payload.is_object()) {
        throw DomainError("owner command target, action, and object payload are required");
    }
    AgentCard target;
    bool found = false;
    for (const auto& card : a2a_.discover("", now_unix)) {
        if (card.agent_id == target_agent_id) {
            target = card;
            found = true;
            break;
        }
    }
    if (!found) {
        throw DomainError("owner command target has no discovered valid Agent Card");
    }
    const auto request_id = "owner:" + Crypto::randomHex(16);
    {
        std::lock_guard lock(owner_mutex_);
        if (owner_pending_.size() >= 1024) {
            throw DomainError("owner channel has too many retained requests");
        }
        owner_pending_.emplace(request_id, target);
    }
    const Json request{{"protocol", "bonded-inbox/owner-channel/v1"},
                       {"requestId", request_id},
                       {"action", action},
                       {"payload", payload},
                       {"replyEncryptionPublicKey", encryption_public_key_}};
    SignedEnvelope envelope{"bonded-inbox/envelope/v2",
                            network_,
                            request_id,
                            agent_id_,
                            target.agent_id,
                            ownerRequestTopic(target.agent_id),
                            Crypto::randomHex(16),
                            ownerMessageExpiry(now_unix),
                            "", "", "", "", "", ""};
    envelope = MessagingService::sealAndSign(
        std::move(envelope), request.dump(),
        target.capabilities.at("messaging_encryption_public_key").get<std::string>(),
        private_key_, public_key_);
    std::string message_id;
    try {
        message_id = messaging_.send(envelope, now_unix);
    } catch (...) {
        std::lock_guard lock(owner_mutex_);
        owner_pending_.erase(request_id);
        throw;
    }
    if (database_ != nullptr) {
        database_->upsertRuntimeRecord(
            "owner-pending", request_id,
            Json{{"requestId", request_id}, {"card", target}});
    }
    return Json{{"requestId", request_id},
                {"messageId", message_id},
                {"state", "pending"}};
}

void SkillRuntime::receiveOwnerRequest(const std::string& raw)
{
    try {
        const auto envelope = Json::parse(raw).get<SignedEnvelope>();
        const auto owner_agent_id = "npk:" + Crypto::sha256(owner_public_key_).substr(0, 32);
        if (envelope.topic != ownerRequestTopic(agent_id_)) return;
        const auto plaintext = MessagingService::open(
            envelope, unixNow(), network_, agent_id_, encryption_private_key_,
            owner_agent_id, owner_public_key_);
        const auto request = Json::parse(plaintext);
        if (request.value("protocol", "") != "bonded-inbox/owner-channel/v1" ||
            request.value("requestId", "") != envelope.id ||
            !request.contains("payload") || !request.at("payload").is_object()) {
            return;
        }
        const auto reply_key = request.at("replyEncryptionPublicKey").get<std::string>();
        requireKey(reply_key, "owner reply encryption public key");
        const auto replay_id = envelope.id + ":" + envelope.nonce;
        std::string expired_replay;
        {
            std::lock_guard lock(owner_mutex_);
            if (!owner_processed_.insert(replay_id).second) return;
            if (owner_processed_.size() > 1024) {
                expired_replay = *owner_processed_.begin();
                owner_processed_.erase(owner_processed_.begin());
            }
        }
        if (database_ != nullptr) {
            database_->upsertRuntimeRecord(
                "owner-processed", replay_id,
                Json{{"envelopeId", replay_id}, {"processedAt", unixNow()}});
            if (!expired_replay.empty()) {
                database_->deleteRuntimeRecord("owner-processed", expired_replay);
            }
        }
        OwnerCommand handler;
        {
            std::lock_guard lock(owner_mutex_);
            handler = owner_command_;
        }
        Json response{{"protocol", "bonded-inbox/owner-channel/v1"},
                      {"requestId", envelope.id}};
        try {
            if (!handler) throw DomainError("agent owner command handler is unavailable");
            response["ok"] = true;
            response["result"] = handler(request.at("action").get<std::string>(),
                                         request.at("payload"));
        } catch (const std::exception& error) {
            response["ok"] = false;
            response["error"] = Json{{"message", error.what()}};
        }
        const auto now = unixNow();
        SignedEnvelope reply{"bonded-inbox/envelope/v2",
                             network_,
                             envelope.id,
                             agent_id_,
                             owner_agent_id,
                             ownerResponseTopic(owner_agent_id),
                             Crypto::randomHex(16),
                             ownerMessageExpiry(now),
                             "", "", "", "", "", ""};
        reply = MessagingService::sealAndSign(std::move(reply), response.dump(), reply_key,
                                              private_key_, public_key_);
        static_cast<void>(messaging_.send(reply, now));
    } catch (const std::exception&) {
        return;
    }
}

void SkillRuntime::receiveOwnerResponse(const std::string& raw)
{
    try {
        const auto envelope = Json::parse(raw).get<SignedEnvelope>();
        if (envelope.topic != ownerResponseTopic(agent_id_)) return;
        AgentCard target;
        {
            std::lock_guard lock(owner_mutex_);
            const auto pending = owner_pending_.find(envelope.id);
            if (pending == owner_pending_.end()) return;
            target = pending->second;
        }
        const auto plaintext = MessagingService::open(
            envelope, unixNow(), network_, agent_id_, encryption_private_key_,
            target.agent_id, target.public_key);
        auto response = Json::parse(plaintext);
        if (response.value("protocol", "") != "bonded-inbox/owner-channel/v1" ||
            response.value("requestId", "") != envelope.id ||
            !response.contains("ok") || !response.at("ok").is_boolean()) {
            return;
        }
        response["receivedAt"] = unixNow();
        std::string expired_response;
        {
            std::lock_guard lock(owner_mutex_);
            owner_responses_[envelope.id] = response;
            owner_pending_.erase(envelope.id);
            while (owner_responses_.size() > 1024) {
                expired_response = owner_responses_.begin()->first;
                owner_responses_.erase(owner_responses_.begin());
            }
        }
        if (database_ != nullptr) {
            database_->upsertRuntimeRecord("owner-response", envelope.id, response);
            database_->deleteRuntimeRecord("owner-pending", envelope.id);
            if (!expired_response.empty()) {
                database_->deleteRuntimeRecord("owner-response", expired_response);
            }
        }
    } catch (const std::exception&) {
        return;
    }
}

Json SkillRuntime::ownerResponses() const
{
    std::lock_guard lock(owner_mutex_);
    Json responses = Json::array();
    for (const auto& [request_id, response] : owner_responses_) {
        (void)request_id;
        responses.push_back(response);
    }
    return responses;
}

} // namespace bonded
