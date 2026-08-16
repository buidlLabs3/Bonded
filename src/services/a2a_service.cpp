#include "services/a2a_service.h"

#include "security/crypto.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>

namespace bonded {
namespace {

constexpr const char* discovery_topic = "/bonded-inbox/1/a2a-card/json";
constexpr const char* task_topic = "/bonded-inbox/1/a2a-task/json";
constexpr const char* a2a_protocol = "a2a/1.0";
constexpr const char* messaging_binding = "LOGOS-MESSAGING";
constexpr const char* messaging_extension =
    "https://github.com/buidlLabs3/Bonded/extensions/logos-messaging/v1";
constexpr const char* payment_extension =
    "https://github.com/buidlLabs3/Bonded/extensions/lez-payment/v1";
constexpr const char* task_extension =
    "https://github.com/buidlLabs3/Bonded/extensions/logos-task/v1";

bool validPrivatePaymentKeys(const Json& keys)
{
    try {
        if (!keys.is_object()) return false;
        const auto nullifier_key = keys.value("nullifier_public_key", "");
        const auto viewing_key = keys.value("viewing_public_key", "");
        return nullifier_key.size() == 64 && Crypto::hexDecode(nullifier_key).size() == 32 &&
               viewing_key.size() % 2 == 0 &&
               (viewing_key.empty() || !Crypto::hexDecode(viewing_key).empty());
    } catch (const std::exception&) {
        return false;
    }
}

std::string base64UrlEncode(const unsigned char* data, std::size_t size)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;
    encoded.reserve((size * 4 + 2) / 3);
    for (std::size_t index = 0; index < size; index += 3) {
        const auto remaining = size - index;
        const auto block = (static_cast<std::uint32_t>(data[index]) << 16U) |
                           (remaining > 1
                                ? static_cast<std::uint32_t>(data[index + 1]) << 8U
                                : 0U) |
                           (remaining > 2 ? static_cast<std::uint32_t>(data[index + 2]) : 0U);
        encoded.push_back(alphabet[(block >> 18U) & 0x3fU]);
        encoded.push_back(alphabet[(block >> 12U) & 0x3fU]);
        if (remaining > 1) encoded.push_back(alphabet[(block >> 6U) & 0x3fU]);
        if (remaining > 2) encoded.push_back(alphabet[block & 0x3fU]);
    }
    return encoded;
}

std::string base64UrlEncode(const std::string& value)
{
    return base64UrlEncode(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::vector<unsigned char> base64UrlDecode(const std::string& value)
{
    if (value.find('=') != std::string::npos || value.size() % 4 == 1) {
        throw DomainError("invalid unpadded base64url value");
    }
    auto sextet = [](char character) -> std::uint32_t {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '-') return 62;
        if (character == '_') return 63;
        throw DomainError("invalid base64url value");
    };
    std::vector<unsigned char> decoded;
    decoded.reserve(value.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto character : value) {
        buffer = (buffer << 6U) | sextet(character);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<unsigned char>((buffer >> bits) & 0xffU));
        }
    }
    if ((bits == 2 && (buffer & 0x03U) != 0) ||
        (bits == 4 && (buffer & 0x0fU) != 0)) {
        throw DomainError("non-canonical base64url value");
    }
    return decoded;
}

std::string protectedHeader(const AgentCard& card)
{
    return base64UrlEncode(Json{{"alg", "EdDSA"},
                                {"kid", card.agent_id},
                                {"typ", "JOSE"}}
                               .dump());
}

Json logosExtensions(const AgentCard& card)
{
    return Json::array(
        {Json{{"uri", messaging_extension},
              {"description", "Encrypted A2A transport over Logos Messaging"},
              {"required", true},
              {"params",
               Json{{"network", card.network},
                    {"agentId", card.agent_id},
                    {"signingPublicKey", card.public_key},
                    {"encryptionPublicKey",
                     card.capabilities.value("messaging_encryption_public_key", "")},
                    {"topic", card.topic},
                    {"expiresAt", card.expires_at}}}},
         Json{{"uri", payment_extension},
              {"description", "Optional LEZ settlement for completed A2A tasks"},
              {"required", card.task_price > 0},
              {"params",
               Json{{"asset", "LEZ"},
                    {"amount", card.task_price},
                    {"recipient", card.capabilities.value("payment_recipient", "")},
                    {"recipientPrivateKeys",
                     card.capabilities.value("payment_private_keys", Json::object())}}}}});
}

Json cardDocument(const AgentCard& card, bool include_signature)
{
    Json skills = Json::array();
    for (const auto& skill : card.skills) {
        skills.push_back(Json{{"id", skill},
                              {"name", skill},
                              {"description", "Bonded agent skill: " + skill},
                              {"tags", Json::array({"logos", "lez"})}});
    }
    Json document{{"name", card.agent_id},
                  {"description", "Bonded privacy agent on " + card.network},
                  {"supportedInterfaces",
                   Json::array({Json{{"url", "logos-messaging:" + card.topic},
                                     {"protocolBinding", messaging_binding},
                                     {"protocolVersion", "1.0"}}})},
                  {"provider",
                   Json{{"organization", "buidlLabs3"},
                        {"url", "https://github.com/buidlLabs3/Bonded"}}},
                  {"version", "0.1.0"},
                  {"documentationUrl", "https://github.com/buidlLabs3/Bonded"},
                  {"capabilities",
                   Json{{"streaming", true},
                        {"pushNotifications", true},
                        {"extensions", logosExtensions(card)}}},
                  {"defaultInputModes", Json::array({"application/json"})},
                  {"defaultOutputModes", Json::array({"application/json"})},
                  {"skills", std::move(skills)}};
    if (include_signature && !card.signature.empty()) {
        const auto separator = card.signature.find('.');
        if (separator == std::string::npos || card.signature.find('.', separator + 1) !=
                                                  std::string::npos) {
            throw DomainError("invalid Agent Card JWS");
        }
        document["signatures"] = Json::array(
            {Json{{"protected", card.signature.substr(0, separator)},
                  {"signature", card.signature.substr(separator + 1)}}});
    }
    return document;
}

Json extensionParams(const Json& card, const std::string& uri)
{
    for (const auto& extension :
         card.at("capabilities").value("extensions", Json::array())) {
        if (extension.value("uri", "") == uri) return extension.at("params");
    }
    throw DomainError("Agent Card is missing a required Logos extension");
}

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
    if (state == "working" || state == "TASK_STATE_WORKING") return A2ATaskState::Working;
    if (state == "input-required" || state == "TASK_STATE_INPUT_REQUIRED") {
        return A2ATaskState::InputRequired;
    }
    if (state == "completed" || state == "TASK_STATE_COMPLETED") {
        return A2ATaskState::Completed;
    }
    if (state == "failed" || state == "TASK_STATE_FAILED") return A2ATaskState::Failed;
    if (state == "canceled" || state == "TASK_STATE_CANCELED") {
        return A2ATaskState::Canceled;
    }
    throw DomainError("unknown persisted A2A task state");
}

std::string a2aTaskState(A2ATaskState state)
{
    switch (state) {
    case A2ATaskState::Working: return "TASK_STATE_WORKING";
    case A2ATaskState::InputRequired: return "TASK_STATE_INPUT_REQUIRED";
    case A2ATaskState::Completed: return "TASK_STATE_COMPLETED";
    case A2ATaskState::Failed: return "TASK_STATE_FAILED";
    case A2ATaskState::Canceled: return "TASK_STATE_CANCELED";
    }
    throw DomainError("unknown A2A task state");
}

std::string a2aOperation(const std::string& action)
{
    if (action == "create") return "SendMessage";
    if (action == "cancel") return "CancelTask";
    return "StreamResponse";
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
            if (verifyCard(card, 0)) {
                cards_.emplace(card.agent_id, card);
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
            if (message.value("protocol", "") != a2a_protocol ||
                message.value("operation", "") != "AgentCard") {
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
    return cardDocument(card, false).dump();
}

AgentCard A2AService::signCard(AgentCard card, const std::string& private_key)
{
    const auto header = protectedHeader(card);
    const auto payload = base64UrlEncode(canonicalCard(card));
    const auto signature = Crypto::hexDecode(
        Crypto::signEd25519(private_key, header + "." + payload));
    card.signature = header + "." + base64UrlEncode(signature.data(), signature.size());
    return card;
}

bool A2AService::verifyCard(const AgentCard& card, std::uint64_t now_unix) const
{
    try {
        const auto separator = card.signature.find('.');
        if (card.protocol != a2a_protocol || card.network != network_ ||
            card.agent_id.empty() || card.public_key.empty() || card.skills.empty() ||
            card.topic.empty() || card.expires_at < now_unix ||
            card.capabilities.value("messaging_encryption_public_key", "").size() != 64 ||
            separator == std::string::npos ||
            card.signature.find('.', separator + 1) != std::string::npos ||
            card.signature.substr(0, separator) != protectedHeader(card)) {
            return false;
        }
        if (card.task_price > 0 &&
            (card.capabilities.value("payment_recipient", "").size() != 64 ||
             !validPrivatePaymentKeys(
                 card.capabilities.value("payment_private_keys", Json::object())))) {
            return false;
        }
        const auto signature = base64UrlDecode(card.signature.substr(separator + 1));
        const auto signature_hex = Crypto::hexEncode(signature.data(), signature.size());
        const auto input = card.signature.substr(0, separator) + "." +
                           base64UrlEncode(canonicalCard(card));
        return Crypto::verifyEd25519(card.public_key, input, signature_hex);
    } catch (const std::exception&) {
        return false;
    }
}

AgentCard A2AService::publishCard(const AgentCard& card, std::uint64_t now_unix)
{
    if (!verifyCard(card, now_unix)) {
        throw DomainError("invalid or expired A2A Agent Card");
    }
    rememberCard(card);
    messaging_.send(discovery_topic,
                    Json{{"protocol", a2a_protocol},
                         {"operation", "AgentCard"},
                         {"card", card}}
                        .dump());
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
        Json{{"protocol", a2a_protocol},
             {"operation", a2aOperation(action)},
             {"logosAction", action},
             {"task", task}}
            .dump(),
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
        if (message.at("protocol") != a2a_protocol) return;
        const auto action = message.at("logosAction").get<std::string>();
        if (message.value("operation", "") != a2aOperation(action)) return;
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
            const auto recipient_private_keys =
                sender.capabilities.at("payment_private_keys");
            const auto proposal = settle_task_(recipient, recipient_private_keys,
                                               task.price, unixNow(),
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
    json = cardDocument(card, true);
}

void from_json(const Json& json, AgentCard& card)
{
    if (json.contains("agent_id")) {
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
        return;
    }

    card.protocol = a2a_protocol;
    card.agent_id = json.at("name").get<std::string>();
    const auto messaging = extensionParams(json, messaging_extension);
    const auto payment = extensionParams(json, payment_extension);
    card.network = messaging.at("network").get<std::string>();
    if (messaging.at("agentId").get<std::string>() != card.agent_id) {
        throw DomainError("Agent Card name and Logos agent id differ");
    }
    card.public_key = messaging.at("signingPublicKey").get<std::string>();
    card.topic = messaging.at("topic").get<std::string>();
    card.expires_at = messaging.at("expiresAt").get<std::uint64_t>();
    card.task_price = payment.value("amount", std::uint64_t{0});
    card.capabilities =
        Json{{"streaming", json.at("capabilities").value("streaming", false)},
             {"paid_tasks", card.task_price > 0},
             {"messaging_encryption", "x25519-aes-256-gcm"},
             {"messaging_encryption_public_key",
              messaging.at("encryptionPublicKey").get<std::string>()},
             {"payment_recipient", payment.value("recipient", "")},
             {"payment_private_keys",
              payment.value("recipientPrivateKeys", Json::object())}};
    card.skills.clear();
    for (const auto& skill : json.at("skills")) {
        card.skills.push_back(skill.at("id").get<std::string>());
    }
    if (json.at("supportedInterfaces").empty() ||
        json.at("supportedInterfaces").front().value("protocolBinding", "") !=
            messaging_binding ||
        json.value("version", "").empty()) {
        throw DomainError("unsupported A2A Agent Card interface");
    }
    const auto& signature = json.at("signatures").at(0);
    card.signature = signature.at("protected").get<std::string>() + "." +
                     signature.at("signature").get<std::string>();
}

void to_json(Json& json, const A2ATask& task)
{
    const Json metadata{{"logos",
                         Json{{"requester", task.requester},
                              {"provider", task.provider},
                              {"skill", task.skill},
                              {"price", task.price},
                              {"expiresAt", task.expires_at},
                              {"paymentReference", task.payment_reference},
                              {"revision", task.revision}}}};
    json = Json{{"id", task.id},
                {"contextId", task.id},
                {"status", Json{{"state", a2aTaskState(task.state)}}},
                {"history",
                 Json::array({Json{{"messageId", "request:" + task.id},
                                   {"contextId", task.id},
                                   {"taskId", task.id},
                                   {"role", "ROLE_USER"},
                                   {"parts",
                                    Json::array({Json{{"data", task.input},
                                                      {"mediaType", "application/json"}}})},
                                   {"extensions", Json::array({task_extension})}}})},
                {"metadata", metadata}};
    if (!task.output.empty()) {
        json["artifacts"] =
            Json::array({Json{{"artifactId", "result:" + task.id},
                              {"parts",
                               Json::array({Json{{"data", task.output},
                                                 {"mediaType", "application/json"}}})}}});
    }
}

void from_json(const Json& json, A2ATask& task)
{
    json.at("id").get_to(task.id);
    if (json.contains("requester")) {
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
        return;
    }

    if (json.value("contextId", "") != task.id) {
        throw DomainError("A2A task context id must match its id");
    }
    const auto& metadata = json.at("metadata").at("logos");
    task.requester = metadata.at("requester").get<std::string>();
    task.provider = metadata.at("provider").get<std::string>();
    task.skill = metadata.at("skill").get<std::string>();
    task.price = metadata.value("price", std::uint64_t{0});
    task.expires_at = metadata.at("expiresAt").get<std::uint64_t>();
    task.payment_reference = metadata.value("paymentReference", "");
    task.revision = metadata.value("revision", std::uint64_t{0});
    task.state = taskState(json.at("status").at("state").get<std::string>());
    task.input = json.at("history").at(0).at("parts").at(0).at("data");
    task.output = json.contains("artifacts")
                      ? json.at("artifacts").at(0).at("parts").at(0).at("data")
                      : Json::object();
}

} // namespace bonded
