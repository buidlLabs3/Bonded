#include "domain/state_machine.h"

#include <algorithm>
#include <array>

namespace bonded {
namespace {

using Edge = std::pair<MessageState, MessageState>;

constexpr std::array<Edge, 10> kEdges{{
    {MessageState::Created, MessageState::BondPending},
    {MessageState::BondPending, MessageState::Bonded},
    {MessageState::BondPending, MessageState::DeliveryFailed},
    {MessageState::Bonded, MessageState::DeliveryPending},
    {MessageState::DeliveryPending, MessageState::PendingReview},
    {MessageState::DeliveryPending, MessageState::DeliveryFailed},
    {MessageState::PendingReview, MessageState::Accepted},
    {MessageState::PendingReview, MessageState::Rejected},
    {MessageState::PendingReview, MessageState::Expired},
    {MessageState::Accepted, MessageState::Settled},
}};

bool isAdditionalTerminalEdge(MessageState from, MessageState to)
{
    return to == MessageState::Settled &&
           (from == MessageState::Rejected || from == MessageState::Expired ||
            from == MessageState::DeliveryFailed);
}

} // namespace

bool MessageStateMachine::canTransition(MessageState from, MessageState to)
{
    return std::find(kEdges.begin(), kEdges.end(), Edge{from, to}) != kEdges.end() ||
           isAdditionalTerminalEdge(from, to);
}

void MessageStateMachine::transition(MessageRecord& message, MessageState to,
                                     std::uint64_t expected_revision)
{
    if (message.revision != expected_revision) {
        throw DomainError("message revision conflict");
    }
    if (!canTransition(message.state, to)) {
        throw DomainError("illegal message transition: " + toString(message.state) + " -> " +
                          toString(to));
    }
    if (to == MessageState::Settled && !message.settlement.has_value()) {
        throw DomainError("settlement outcome is required before settling a message");
    }
    message.state = to;
    ++message.revision;
}

std::vector<MessageState> MessageStateMachine::allowedTransitions(MessageState from)
{
    std::vector<MessageState> result;
    for (const auto& [edge_from, edge_to] : kEdges) {
        if (edge_from == from) {
            result.push_back(edge_to);
        }
    }
    if (isAdditionalTerminalEdge(from, MessageState::Settled)) {
        result.push_back(MessageState::Settled);
    }
    return result;
}

bool MessageStateMachine::isDecision(MessageState state)
{
    return state == MessageState::Accepted || state == MessageState::Rejected ||
           state == MessageState::Expired || state == MessageState::DeliveryFailed;
}

SettlementOutcome MessageStateMachine::requiredSettlement(MessageState decision)
{
    switch (decision) {
    case MessageState::Accepted:
        return SettlementOutcome::RefundAccepted;
    case MessageState::Rejected:
        return SettlementOutcome::SinkRejected;
    case MessageState::Expired:
        return SettlementOutcome::RefundExpired;
    case MessageState::DeliveryFailed:
        return SettlementOutcome::RefundDeliveryFailed;
    default:
        throw DomainError("message state has no settlement outcome");
    }
}

std::string toString(MessageState state)
{
    switch (state) {
    case MessageState::Created:
        return "created";
    case MessageState::BondPending:
        return "bond_pending";
    case MessageState::Bonded:
        return "bonded";
    case MessageState::DeliveryPending:
        return "delivery_pending";
    case MessageState::PendingReview:
        return "pending_review";
    case MessageState::Accepted:
        return "accepted";
    case MessageState::Rejected:
        return "rejected";
    case MessageState::Expired:
        return "expired";
    case MessageState::DeliveryFailed:
        return "delivery_failed";
    case MessageState::Settled:
        return "settled";
    }
    throw DomainError("unknown message state");
}

MessageState messageStateFromString(const std::string& value)
{
    static const std::array<std::pair<const char*, MessageState>, 10> values{{
        {"created", MessageState::Created},
        {"bond_pending", MessageState::BondPending},
        {"bonded", MessageState::Bonded},
        {"delivery_pending", MessageState::DeliveryPending},
        {"pending_review", MessageState::PendingReview},
        {"accepted", MessageState::Accepted},
        {"rejected", MessageState::Rejected},
        {"expired", MessageState::Expired},
        {"delivery_failed", MessageState::DeliveryFailed},
        {"settled", MessageState::Settled},
    }};
    const auto found = std::find_if(values.begin(), values.end(), [&](const auto& item) {
        return value == item.first;
    });
    if (found == values.end()) {
        throw DomainError("invalid message state: " + value);
    }
    return found->second;
}

std::string toString(SettlementOutcome outcome)
{
    switch (outcome) {
    case SettlementOutcome::RefundAccepted:
        return "refund_accepted";
    case SettlementOutcome::SinkRejected:
        return "sink_rejected";
    case SettlementOutcome::RefundExpired:
        return "refund_expired";
    case SettlementOutcome::RefundDeliveryFailed:
        return "refund_delivery_failed";
    }
    throw DomainError("unknown settlement outcome");
}

SettlementOutcome settlementOutcomeFromString(const std::string& value)
{
    if (value == "refund_accepted") {
        return SettlementOutcome::RefundAccepted;
    }
    if (value == "sink_rejected") {
        return SettlementOutcome::SinkRejected;
    }
    if (value == "refund_expired") {
        return SettlementOutcome::RefundExpired;
    }
    if (value == "refund_delivery_failed") {
        return SettlementOutcome::RefundDeliveryFailed;
    }
    throw DomainError("invalid settlement outcome: " + value);
}

std::string toString(Profile profile)
{
    switch (profile) {
    case Profile::Inbox:
        return "inbox";
    case Profile::Vault:
        return "vault";
    case Profile::Settlement:
        return "settlement";
    }
    throw DomainError("unknown profile");
}

Profile profileFromString(const std::string& value)
{
    if (value == "inbox") {
        return Profile::Inbox;
    }
    if (value == "vault") {
        return Profile::Vault;
    }
    if (value == "settlement") {
        return Profile::Settlement;
    }
    throw DomainError("invalid profile: " + value);
}

void to_json(Json& json, const AttachmentPolicy& value)
{
    json = Json{{"allowed_types", value.allowed_types},
                {"max_bytes", value.max_bytes},
                {"max_count", value.max_count}};
}

void from_json(const Json& json, AttachmentPolicy& value)
{
    json.at("allowed_types").get_to(value.allowed_types);
    json.at("max_bytes").get_to(value.max_bytes);
    json.at("max_count").get_to(value.max_count);
}

void to_json(Json& json, const InboxPolicy& value)
{
    json = Json{{"inbox_id", value.inbox_id},
                {"owner_address", value.owner_address},
                {"sink_address", value.sink_address},
                {"emergency_channel", value.emergency_channel},
                {"network", value.network},
                {"version", value.version},
                {"bond_amount", value.bond_amount},
                {"response_timeout_seconds", value.response_timeout_seconds},
                {"valid_from_unix", value.valid_from_unix},
                {"attachments", value.attachments},
                {"signer_public_key", value.signer_public_key},
                {"signature", value.signature}};
}

void from_json(const Json& json, InboxPolicy& value)
{
    json.at("inbox_id").get_to(value.inbox_id);
    json.at("owner_address").get_to(value.owner_address);
    json.at("sink_address").get_to(value.sink_address);
    json.at("emergency_channel").get_to(value.emergency_channel);
    json.at("network").get_to(value.network);
    json.at("version").get_to(value.version);
    json.at("bond_amount").get_to(value.bond_amount);
    json.at("response_timeout_seconds").get_to(value.response_timeout_seconds);
    json.at("valid_from_unix").get_to(value.valid_from_unix);
    json.at("attachments").get_to(value.attachments);
    value.signer_public_key = json.value("signer_public_key", "");
    value.signature = json.value("signature", "");
}

void to_json(Json& json, const MessageRecord& value)
{
    json = Json{{"id", value.id},
                {"sender", value.sender},
                {"policy_hash", value.policy_hash},
                {"policy_version", value.policy_version},
                {"bond_amount", value.bond_amount},
                {"deadline_unix", value.deadline_unix},
                {"state", toString(value.state)},
                {"revision", value.revision}};
    if (value.settlement.has_value()) {
        json["settlement"] = toString(*value.settlement);
    }
}

void from_json(const Json& json, MessageRecord& value)
{
    json.at("id").get_to(value.id);
    json.at("sender").get_to(value.sender);
    json.at("policy_hash").get_to(value.policy_hash);
    json.at("policy_version").get_to(value.policy_version);
    json.at("bond_amount").get_to(value.bond_amount);
    json.at("deadline_unix").get_to(value.deadline_unix);
    value.state = messageStateFromString(json.at("state").get<std::string>());
    json.at("revision").get_to(value.revision);
    if (json.contains("settlement")) {
        value.settlement = settlementOutcomeFromString(json.at("settlement").get<std::string>());
    }
}

void to_json(Json& json, const BondRecord& value)
{
    json = Json{{"id", value.id},
                {"message_id", value.message_id},
                {"sender", value.sender},
                {"owner", value.owner},
                {"sink", value.sink},
                {"policy_hash", value.policy_hash},
                {"amount", value.amount},
                {"deadline_unix", value.deadline_unix}};
    if (value.outcome.has_value()) {
        json["outcome"] = toString(*value.outcome);
    }
}

void from_json(const Json& json, BondRecord& value)
{
    json.at("id").get_to(value.id);
    json.at("message_id").get_to(value.message_id);
    json.at("sender").get_to(value.sender);
    json.at("owner").get_to(value.owner);
    json.at("sink").get_to(value.sink);
    json.at("policy_hash").get_to(value.policy_hash);
    json.at("amount").get_to(value.amount);
    json.at("deadline_unix").get_to(value.deadline_unix);
    if (json.contains("outcome")) {
        value.outcome = settlementOutcomeFromString(json.at("outcome").get<std::string>());
    }
}

} // namespace bonded
