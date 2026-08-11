#include "runtime/skill_runtime.h"

#include "runtime/default_skill_catalog.h"
#include "security/crypto.h"

#include <algorithm>

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

} // namespace

SkillRuntime::SkillRuntime(SkillRegistry& registry, Profile profile, const Json& configuration,
                           std::function<void(const Json&)> owner_action_required)
    : registry_(registry), profile_(profile),
      network_(configuration.value("network", "logos-local")),
      storage_key_(Crypto::randomHex(32)),
      owner_action_required_(std::move(owner_action_required)),
      wallet_adapter_(configuration.value("initial_balance", std::uint64_t{1000})),
      messaging_(messaging_adapter_, network_), storage_(storage_adapter_),
      spending_(wallet_adapter_,
                SpendingPolicy{configuration.value("per_transaction_limit", std::uint64_t{100}),
                               configuration.value("per_period_limit", std::uint64_t{500}),
                               configuration.value("period_seconds", std::uint64_t{86400}),
                               configuration.value("approval_timeout_seconds",
                                                   std::uint64_t{3600})}),
      a2a_(program_adapter_, network_),
      configuration_(Json{{"classifier_enabled", configuration.value("classifier_enabled", true)},
                          {"rate_limit", configuration.value("rate_limit", std::uint64_t{10})},
                          {"owner_notifications",
                           configuration.value("owner_notifications", true)},
                          {"approval_timeout_seconds",
                           configuration.value("approval_timeout_seconds",
                                               std::uint64_t{3600})}},
                     configuration.value("owner_public_key", ""))
{
    const auto keys = Crypto::generateEd25519KeyPair();
    private_key_ = keys.first;
    public_key_ = keys.second;
    agent_id_ = "npk:" + Crypto::sha256(public_key_).substr(0, 32);
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
                                std::uint64_t task_price) const
{
    if (expires_at <= now_unix) {
        throw DomainError("Agent Card expiry must be in the future");
    }
    AgentCard card{"lf.a2a.v1",
                   network_,
                   agent_id_,
                   public_key_,
                   profileNames(registry_, profile_),
                   Json{{"streaming", true}, {"paid_tasks", true}},
                   "bonded/a2a/" + agent_id_,
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
        SignedEnvelope envelope{"bonded-inbox/envelope/v1",
                                network_,
                                input.value("id", Crypto::randomHex(16)),
                                agent_id_,
                                input.at("recipient").get<std::string>(),
                                input.at("topic").get<std::string>(),
                                input.at("payload").get<std::string>(),
                                Crypto::randomHex(16),
                                input.value("expires_at", now + 300),
                                "",
                                ""};
        envelope = MessagingService::sign(std::move(envelope), private_key_, public_key_);
        return Json{{"message_id", messaging_.send(envelope, now)}};
    }
    if (name == "messaging.join") {
        messaging_adapter_.join(input.at("group_id").get<std::string>());
        return Json{{"joined", true}};
    }
    if (name == "messaging.create_group") {
        return Json{{"group_id", messaging_adapter_.createGroup(members(input))}};
    }
    if (name == "wallet.balance") {
        return Json{{"balance", wallet_adapter_.balance()}, {"asset", "LEZ"}};
    }
    if (name == "wallet.send") {
        const auto proposal = spending_.propose(input.at("recipient").get<std::string>(),
                                                input.at("amount").get<std::uint64_t>(),
                                                input.at("now_unix").get<std::uint64_t>());
        const auto output = spendingProposalJson(proposal);
        if (proposal.state == ApprovalState::Pending && owner_action_required_) {
            owner_action_required_(Json{{"type", "spending.approval_required"},
                                        {"proposal", output}});
        }
        return output;
    }
    if (name == "wallet.history") {
        Json history = Json::array();
        for (const auto& transfer : wallet_adapter_.history()) {
            history.push_back(Json{{"id", transfer.id},
                                   {"recipient", transfer.recipient},
                                   {"amount", transfer.amount},
                                   {"timestamp_unix", transfer.timestamp_unix}});
        }
        return history;
    }
    if (name == "program.query") {
        return program_adapter_.query(input.at("program_id").get<std::string>(),
                                      input.value("parameters", Json::object()));
    }
    if (name == "program.call") {
        return Json{{"call_id", program_adapter_.call(
                                    input.at("program_id").get<std::string>(),
                                    input.at("instruction").get<std::string>(),
                                    input.value("parameters", Json::object()))}};
    }
    if (name == "program.deploy") {
        return Json{{"program_id",
                     program_adapter_.deploy(input.at("binary_path").get<std::string>())}};
    }
    if (name == "agent.card") {
        const auto now = input.value("now_unix", std::uint64_t{0});
        if (input.contains("card")) {
            return a2a_.publishCard(input.at("card").get<AgentCard>(), now);
        }
        const auto card = ownCard(now, input.at("expires_at").get<std::uint64_t>(),
                                  input.value("task_price", std::uint64_t{0}));
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
                                 input.value("output", Json::object()));
        }
        if (action == "fail") {
            return a2a_.fail(input.at("task_id").get<std::string>(),
                             input.at("reason").get<std::string>());
        }
        if (action == "input_required") {
            return a2a_.requireInput(input.at("task_id").get<std::string>());
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
    return Json{{"state", "ready"},
                {"profile", toString(profile_)},
                {"agent_id", agent_id_},
                {"balance", wallet_adapter_.balance()},
                {"storage_bytes", storage_bytes},
                {"active_tasks", a2a_.activeTaskCount()},
                {"configuration", configuration_.snapshot()},
                {"dependencies", Json{{"messaging", "local-adapter"},
                                       {"storage", "local-adapter"},
                                       {"wallet", "local-adapter"},
                                       {"program", "local-adapter"}}}};
}

} // namespace bonded
