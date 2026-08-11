#include "services/a2a_service.h"

#include "security/crypto.h"

#include <algorithm>

namespace bonded {

A2AService::A2AService(ProgramAdapter& program, std::string network)
    : program_(program), network_(std::move(network))
{
    if (network_.empty()) {
        throw DomainError("A2A network is required");
    }
}

std::string A2AService::canonicalCard(const AgentCard& card)
{
    Json json = card;
    json.erase("signature");
    return json.dump();
}

AgentCard A2AService::signCard(AgentCard card, const std::string& private_key)
{
    card.signature = Crypto::signEd25519(private_key, canonicalCard(card));
    return card;
}

bool A2AService::verifyCard(const AgentCard& card, std::uint64_t now_unix) const
{
    return card.protocol == "lf.a2a.v1" && card.network == network_ &&
           !card.agent_id.empty() && !card.public_key.empty() && !card.skills.empty() &&
           !card.topic.empty() && card.expires_at >= now_unix &&
           Crypto::verifyEd25519(card.public_key, canonicalCard(card), card.signature);
}

AgentCard A2AService::publishCard(const AgentCard& card, std::uint64_t now_unix)
{
    if (!verifyCard(card, now_unix)) {
        throw DomainError("invalid or expired A2A Agent Card");
    }
    std::lock_guard lock(mutex_);
    cards_[card.agent_id] = card;
    return card;
}

std::vector<AgentCard> A2AService::discover(const std::string& skill,
                                           std::uint64_t now_unix) const
{
    std::lock_guard lock(mutex_);
    std::vector<AgentCard> result;
    for (const auto& [id, card] : cards_) {
        (void)id;
        if (verifyCard(card, now_unix) &&
            (skill.empty() || std::find(card.skills.begin(), card.skills.end(), skill) !=
                                  card.skills.end())) {
            result.push_back(card);
        }
    }
    return result;
}

A2ATask A2AService::createTask(A2ATask task, std::uint64_t now_unix)
{
    if (task.id.empty() || task.requester.empty() || task.provider.empty() || task.skill.empty() ||
        !task.input.is_object() || task.expires_at <= now_unix) {
        throw DomainError("invalid A2A task");
    }
    std::lock_guard lock(mutex_);
    const auto card = cards_.find(task.provider);
    if (card == cards_.end() || !verifyCard(card->second, now_unix) ||
        std::find(card->second.skills.begin(), card->second.skills.end(), task.skill) ==
            card->second.skills.end()) {
        throw DomainError("A2A provider does not advertise the requested skill");
    }
    if (task.price != card->second.task_price) {
        throw DomainError("A2A task price does not match signed Agent Card");
    }
    const auto existing = tasks_.find(task.id);
    if (existing != tasks_.end()) {
        return existing->second;
    }
    task.state = A2ATaskState::Working;
    task.payment_reference = program_.call(
        "bonded-a2a-escrow", "lock",
        Json{{"task_id", task.id}, {"provider", task.provider}, {"amount", task.price}});
    tasks_.emplace(task.id, task);
    return task;
}

A2ATask A2AService::requireInput(const std::string& task_id)
{
    std::lock_guard lock(mutex_);
    auto& task = tasks_.at(task_id);
    if (task.state != A2ATaskState::Working) {
        throw DomainError("A2A task cannot request input in its current state");
    }
    task.state = A2ATaskState::InputRequired;
    ++task.revision;
    return task;
}

A2ATask A2AService::complete(const std::string& task_id, const Json& output)
{
    std::lock_guard lock(mutex_);
    auto& task = tasks_.at(task_id);
    if (task.state == A2ATaskState::Completed) {
        return task;
    }
    if (task.state != A2ATaskState::Working && task.state != A2ATaskState::InputRequired) {
        throw DomainError("A2A task cannot complete in its current state");
    }
    program_.call("bonded-a2a-escrow", "release",
                  Json{{"task_id", task.id}, {"payment_reference", task.payment_reference}});
    task.output = output;
    task.state = A2ATaskState::Completed;
    ++task.revision;
    return task;
}

A2ATask A2AService::fail(const std::string& task_id, const std::string& reason)
{
    std::lock_guard lock(mutex_);
    auto& task = tasks_.at(task_id);
    if (task.state != A2ATaskState::Working && task.state != A2ATaskState::InputRequired) {
        throw DomainError("A2A task cannot fail in its current state");
    }
    program_.call("bonded-a2a-escrow", "refund",
                  Json{{"task_id", task.id}, {"reason", reason}});
    task.output = Json{{"error", reason}};
    task.state = A2ATaskState::Failed;
    ++task.revision;
    return task;
}

A2ATask A2AService::subscribe(const std::string& task_id) const
{
    std::lock_guard lock(mutex_);
    const auto found = tasks_.find(task_id);
    if (found == tasks_.end()) {
        throw DomainError("unknown A2A task");
    }
    return found->second;
}

A2ATask A2AService::cancel(const std::string& task_id, std::uint64_t now_unix)
{
    std::lock_guard lock(mutex_);
    auto& task = tasks_.at(task_id);
    if (task.state == A2ATaskState::Canceled) {
        return task;
    }
    if (task.state != A2ATaskState::Working && task.state != A2ATaskState::InputRequired) {
        throw DomainError("A2A task cannot be canceled in its current state");
    }
    program_.call("bonded-a2a-escrow", "refund",
                  Json{{"task_id", task.id}, {"canceled_at", now_unix}});
    task.state = A2ATaskState::Canceled;
    ++task.revision;
    return task;
}

std::size_t A2AService::activeTaskCount() const
{
    std::lock_guard lock(mutex_);
    return std::count_if(tasks_.begin(), tasks_.end(), [](const auto& item) {
        return item.second.state == A2ATaskState::Working ||
               item.second.state == A2ATaskState::InputRequired;
    });
}

std::string toString(A2ATaskState state)
{
    switch (state) {
    case A2ATaskState::Working:
        return "working";
    case A2ATaskState::InputRequired:
        return "input-required";
    case A2ATaskState::Completed:
        return "completed";
    case A2ATaskState::Failed:
        return "failed";
    case A2ATaskState::Canceled:
        return "canceled";
    }
    throw DomainError("unknown A2A task state");
}

void to_json(Json& json, const AgentCard& card)
{
    json = Json{{"protocol", card.protocol},
                {"network", card.network},
                {"agent_id", card.agent_id},
                {"public_key", card.public_key},
                {"skills", card.skills},
                {"capabilities", card.capabilities},
                {"topic", card.topic},
                {"task_price", card.task_price},
                {"expires_at", card.expires_at},
                {"signature", card.signature}};
}

void from_json(const Json& json, AgentCard& card)
{
    json.at("protocol").get_to(card.protocol);
    json.at("network").get_to(card.network);
    json.at("agent_id").get_to(card.agent_id);
    json.at("public_key").get_to(card.public_key);
    json.at("skills").get_to(card.skills);
    card.capabilities = json.value("capabilities", Json::object());
    json.at("topic").get_to(card.topic);
    card.task_price = json.value("task_price", std::uint64_t{0});
    json.at("expires_at").get_to(card.expires_at);
    json.at("signature").get_to(card.signature);
}

void to_json(Json& json, const A2ATask& task)
{
    json = Json{{"id", task.id},
                {"requester", task.requester},
                {"provider", task.provider},
                {"skill", task.skill},
                {"input", task.input},
                {"output", task.output},
                {"price", task.price},
                {"expires_at", task.expires_at},
                {"state", toString(task.state)},
                {"payment_reference", task.payment_reference},
                {"revision", task.revision}};
}

void from_json(const Json& json, A2ATask& task)
{
    json.at("id").get_to(task.id);
    json.at("requester").get_to(task.requester);
    json.at("provider").get_to(task.provider);
    json.at("skill").get_to(task.skill);
    task.input = json.value("input", Json::object());
    task.output = json.value("output", Json::object());
    task.price = json.value("price", std::uint64_t{0});
    json.at("expires_at").get_to(task.expires_at);
    task.payment_reference = json.value("payment_reference", "");
    task.revision = json.value("revision", std::uint64_t{0});
}

} // namespace bonded
