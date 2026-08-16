#include "services/a2a_service.h"

#include "security/crypto.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace bonded {
namespace {

constexpr const char* discovery_topic = "/bonded-inbox/1/a2a-card/json";
constexpr const char* task_topic = "/bonded-inbox/1/a2a-task/json";

std::uint64_t unixNow()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t transportExpiry(std::uint64_t task_expiry, std::uint64_t now_unix)
{
    constexpr auto lifetime = std::uint64_t{300};
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto message_expiry = now_unix > maximum - lifetime ? maximum : now_unix + lifetime;
    return std::min(task_expiry, message_expiry);
}

A2ATaskState taskState(const std::string& state)
{
    if (state == "working") return A2ATaskState::Working;
    if (state == "input-required") return A2ATaskState::InputRequired;
    if (state == "completed") return A2ATaskState::Completed;
    if (state == "failed") return A2ATaskState::Failed;
    if (state == "canceled") return A2ATaskState::Canceled;
    throw DomainError("unknown persisted A2A task state");
}

} // namespace

A2AService::A2AService(MessagingAdapter& messaging, std::string network,
                       LoadCards load_cards, SaveCard save_card,
                       LoadTasks load_tasks, SaveTask save_task,
                       SettleTask settle_task)
    : messaging_(messaging), network_(std::move(network)),
      transport_(messaging_, network_),
      save_card_(std::move(save_card)), save_task_(std::move(save_task)),
      settle_task_(std::move(settle_task))
{
    if (network_.empty()) {
        throw DomainError("A2A network is required");
    }
    if (load_cards) {
        for (const auto& card : load_cards()) {
            if (!verifyCard(card, 0) || !cards_.emplace(card.agent_id, card).second) {
                throw DomainError("persisted A2A Agent Card is invalid or duplicated");
            }
        }
    }
    if (load_tasks) {
        for (const auto& task : load_tasks()) {
            if (task.id.empty() || !tasks_.emplace(task.id, task).second) {
                throw DomainError("persisted A2A task is invalid or duplicated");
            }
        }
    }
    messaging_.subscribe(discovery_topic, [this](const std::string& payload) {
        try {
            const auto message = Json::parse(payload);
            if (message.value("protocol", "") != "lf.a2a.discovery/v1") {
                return;
            }
            const auto card = message.at("card").get<AgentCard>();
            if (verifyCard(card, 0)) {
                rememberCard(card);
            }
        } catch (const std::exception&) {
            return;
        }
    });
}

void A2AService::configureTransport(const std::string& agent_id,
                                    const std::string& signing_private_key,
                                    const std::string& signing_public_key,
                                    const std::string& encryption_private_key)
{
    if (transport_configured_ || agent_id.empty() || signing_private_key.empty() ||
        signing_public_key.empty() || encryption_private_key.empty()) {
        throw DomainError("A2A transport identity is invalid or already configured");
    }
    agent_id_ = agent_id;
    signing_private_key_ = signing_private_key;
    signing_public_key_ = signing_public_key;
    encryption_private_key_ = encryption_private_key;
    transport_configured_ = true;
    messaging_.subscribe(task_topic, [this](const std::string& payload) {
        receiveTask(payload);
    });
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
    rememberCard(card);
    messaging_.send(discovery_topic,
                    Json{{"protocol", "lf.a2a.discovery/v1"}, {"card", card}}.dump());
    return card;
}

void A2AService::rememberCard(const AgentCard& card)
{
    std::lock_guard lock(mutex_);
    cards_[card.agent_id] = card;
    if (save_card_) save_card_(card);
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
    if (!transport_configured_ || task.id.empty() || task.requester != agent_id_ ||
        task.provider.empty() || task.skill.empty() ||
        !task.input.is_object() || task.expires_at <= now_unix) {
        throw DomainError("invalid A2A task");
    }
    {
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
        tasks_.emplace(task.id, task);
        if (save_task_) save_task_(task);
    }
    sendTask(task, "create", now_unix);
    return task;
}

A2ATask A2AService::requireInput(const std::string& task_id, std::uint64_t now_unix)
{
    A2ATask updated;
    {
        std::lock_guard lock(mutex_);
        auto& task = tasks_.at(task_id);
        if (task.provider != agent_id_ || task.state != A2ATaskState::Working) {
            throw DomainError("A2A task cannot request input in its current state");
        }
        task.state = A2ATaskState::InputRequired;
        ++task.revision;
        if (save_task_) save_task_(task);
        updated = task;
    }
    sendTask(updated, "input-required", now_unix);
    return updated;
}

A2ATask A2AService::complete(const std::string& task_id, const Json& output,
                             std::uint64_t now_unix)
{
    A2ATask updated;
    {
        std::lock_guard lock(mutex_);
        auto& task = tasks_.at(task_id);
        if (task.state == A2ATaskState::Completed) {
            return task;
        }
        if (task.provider != agent_id_ ||
            (task.state != A2ATaskState::Working &&
             task.state != A2ATaskState::InputRequired)) {
            throw DomainError("A2A task cannot complete in its current state");
        }
        task.output = output;
        task.state = A2ATaskState::Completed;
        ++task.revision;
        if (save_task_) save_task_(task);
        updated = task;
    }
    sendTask(updated, "complete", now_unix);
    return updated;
}

A2ATask A2AService::fail(const std::string& task_id, const std::string& reason,
                         std::uint64_t now_unix)
{
    A2ATask updated;
    {
        std::lock_guard lock(mutex_);
        auto& task = tasks_.at(task_id);
        if (task.provider != agent_id_ ||
            (task.state != A2ATaskState::Working &&
             task.state != A2ATaskState::InputRequired)) {
            throw DomainError("A2A task cannot fail in its current state");
        }
        task.output = Json{{"error", reason}};
        task.state = A2ATaskState::Failed;
        ++task.revision;
        if (save_task_) save_task_(task);
        updated = task;
    }
    sendTask(updated, "fail", now_unix);
    return updated;
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
    A2ATask updated;
    {
        std::lock_guard lock(mutex_);
        auto& task = tasks_.at(task_id);
        if (task.state == A2ATaskState::Canceled) {
            return task;
        }
        if (task.requester != agent_id_ ||
            (task.state != A2ATaskState::Working &&
             task.state != A2ATaskState::InputRequired)) {
            throw DomainError("A2A task cannot be canceled in its current state");
        }
        task.state = A2ATaskState::Canceled;
        ++task.revision;
        if (save_task_) save_task_(task);
        updated = task;
    }
    sendTask(updated, "cancel", now_unix);
    return updated;
}

void A2AService::sendTask(const A2ATask& task, const std::string& action,
                          std::uint64_t now_unix)
{
    if (!transport_configured_) {
        throw DomainError("A2A task transport is not configured");
    }
    const auto recipient = agent_id_ == task.requester ? task.provider : task.requester;
    AgentCard card;
    {
        std::lock_guard lock(mutex_);
        const auto found = cards_.find(recipient);
        if (found == cards_.end() || !verifyCard(found->second, now_unix)) {
            throw DomainError("A2A task recipient has no valid Agent Card");
        }
        card = found->second;
    }
    const auto encryption_key =
        card.capabilities.at("messaging_encryption_public_key").get<std::string>();
    SignedEnvelope envelope{"bonded-inbox/envelope/v2", network_,
                            "a2a:" + task.id + ":" + std::to_string(task.revision),
                            agent_id_, recipient, task_topic, Crypto::randomHex(16),
                            transportExpiry(task.expires_at, now_unix),
                            "", "", "", "", "", ""};
    envelope = MessagingService::sealAndSign(
        std::move(envelope),
        Json{{"protocol", "lf.a2a.task/v1"}, {"action", action}, {"task", task}}.dump(),
        encryption_key, signing_private_key_, signing_public_key_);
    static_cast<void>(transport_.send(envelope, now_unix));
}

void A2AService::receiveTask(const std::string& payload)
{
    try {
        const auto envelope = Json::parse(payload).get<SignedEnvelope>();
        if (!transport_configured_ || envelope.recipient != agent_id_ ||
            envelope.topic != task_topic) {
            return;
        }
        AgentCard sender;
        {
            std::lock_guard lock(mutex_);
            const auto found = cards_.find(envelope.sender);
            if (found == cards_.end()) return;
            sender = found->second;
        }
        const auto plaintext = MessagingService::open(
            envelope, unixNow(), network_, agent_id_, encryption_private_key_,
            sender.agent_id, sender.public_key);
        const auto message = Json::parse(plaintext);
        if (message.at("protocol") != "lf.a2a.task/v1") return;
        const auto action = message.at("action").get<std::string>();
        const auto task = message.at("task").get<A2ATask>();
        const auto sender_authorized =
            ((action == "create" || action == "cancel" || action == "payment") &&
             task.requester == envelope.sender) ||
            ((action == "input-required" || action == "complete" || action == "fail") &&
             task.provider == envelope.sender);
        if (task.id.empty() || !sender_authorized ||
            (task.requester != agent_id_ && task.provider != agent_id_)) {
            return;
        }
        bool settle = false;
        {
            std::lock_guard lock(mutex_);
            const auto existing = tasks_.find(task.id);
            if (existing != tasks_.end()) {
                const auto& current = existing->second;
                if (current.requester != task.requester || current.provider != task.provider ||
                    current.skill != task.skill || current.input != task.input ||
                    current.price != task.price || current.expires_at != task.expires_at ||
                    task.revision <= current.revision) {
                    return;
                }
            } else {
                const auto own_card = cards_.find(task.provider);
                if (action != "create" || task.provider != agent_id_ ||
                    own_card == cards_.end() || own_card->second.task_price != task.price ||
                    std::find(own_card->second.skills.begin(), own_card->second.skills.end(),
                              task.skill) == own_card->second.skills.end()) {
                    return;
                }
            }
            settle = action == "complete" && task.requester == agent_id_ &&
                     task.price > 0 && static_cast<bool>(settle_task_);
        }

        auto updated = task;
        if (settle) {
            const auto recipient =
                sender.capabilities.at("payment_recipient").get<std::string>();
            const auto proposal = settle_task_(recipient, task.price, unixNow(),
                                               "a2a:" + task.id);
            const auto state = proposal.at("state").get<std::string>();
            updated.payment_reference =
                state == "executed" ? proposal.at("transfer_id").get<std::string>()
                                    : proposal.at("id").get<std::string>();
            updated.state = state == "executed" ? A2ATaskState::Completed
                                                 : A2ATaskState::InputRequired;
            ++updated.revision;
        }

        {
            std::lock_guard lock(mutex_);
            const auto existing = tasks_.find(updated.id);
            if (existing != tasks_.end() && updated.revision <= existing->second.revision) {
                return;
            }
            tasks_[updated.id] = updated;
            if (save_task_) save_task_(updated);
        }
        if (settle) {
            sendTask(updated, "payment", unixNow());
        }
    } catch (const std::exception&) {
        return;
    }
}

void A2AService::recordSettlement(const std::string& proposal_id,
                                  const std::string& transfer_id,
                                  std::uint64_t now_unix)
{
    if (proposal_id.empty() || transfer_id.empty()) {
        throw DomainError("A2A settlement identifiers are required");
    }
    std::optional<A2ATask> updated;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, task] : tasks_) {
            (void)id;
            if (task.payment_reference == proposal_id &&
                task.state == A2ATaskState::InputRequired) {
                task.payment_reference = transfer_id;
                task.state = A2ATaskState::Completed;
                ++task.revision;
                if (save_task_) save_task_(task);
                updated = task;
                break;
            }
        }
    }
    if (updated.has_value()) {
        sendTask(*updated, "payment", now_unix);
    }
}

std::vector<A2ATask> A2AService::tasks() const
{
    std::lock_guard lock(mutex_);
    std::vector<A2ATask> result;
    result.reserve(tasks_.size());
    for (const auto& [id, task] : tasks_) {
        (void)id;
        result.push_back(task);
    }
    return result;
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
    task.state = taskState(json.at("state").get<std::string>());
}

} // namespace bonded
