#include "services/inbox_service.h"

#include "domain/state_machine.h"

#include <algorithm>

namespace bonded {

InboxService::InboxService(Database& database, BondService& bonds)
    : database_(database), bonds_(bonds)
{
}

InboxPolicy InboxService::publishPolicy(const InboxPolicy& policy)
{
    if (!PolicyService::verify(policy)) {
        throw DomainError("policy signature is invalid");
    }
    const auto current = database_.latestPolicy(policy.inbox_id);
    if (current.has_value()) {
        PolicyService::requireSuccessor(*current, policy);
    }
    database_.savePolicy(policy);
    database_.enqueue("bonded/policy/" + policy.inbox_id, Json(policy).dump());
    return policy;
}

MessageRecord InboxService::submit(const Submission& submission)
{
    if (submission.inbox_id.empty() || submission.message_id.empty() ||
        submission.idempotency_key.empty() || submission.sender.empty()) {
        throw DomainError("inbox, message, idempotency key, and sender are required");
    }
    const auto policy = database_.latestPolicy(submission.inbox_id);
    if (!policy.has_value() || !PolicyService::verify(*policy)) {
        throw DomainError("no valid inbox policy");
    }
    if (PolicyService::hash(*policy) != submission.policy_hash) {
        throw DomainError("submission policy commitment is stale or invalid");
    }
    if (submission.now_unix < policy->valid_from_unix) {
        throw DomainError("policy is not active");
    }
    if (submission.attachment_count > policy->attachments.max_count) {
        throw DomainError("too many attachments");
    }
    if (submission.attachment_count == 1) {
        if (submission.attachment_bytes > policy->attachments.max_bytes ||
            std::find(policy->attachments.allowed_types.begin(), policy->attachments.allowed_types.end(),
                      submission.attachment_type) == policy->attachments.allowed_types.end()) {
            throw DomainError("attachment violates inbox policy");
        }
    }
    if (!submission.trusted_contact && submission.bond_amount != policy->bond_amount) {
        throw DomainError("bond amount does not match policy");
    }
    if (!submission.trusted_contact &&
        submission.bond_id != "bond:" + submission.message_id) {
        throw DomainError("bond identifier must be bound to the message");
    }

    MessageRecord message{submission.message_id,
                          submission.sender,
                          submission.policy_hash,
                          policy->version,
                          submission.trusted_contact ? 0 : policy->bond_amount,
                          submission.now_unix + policy->response_timeout_seconds,
                          MessageState::Created,
                          0,
                          std::nullopt};

    if (!database_.createMessage(message, submission.idempotency_key)) {
        const auto existing = database_.message(submission.message_id);
        if (!existing.has_value()) {
            throw DomainError("idempotency key belongs to another message");
        }
        return *existing;
    }

    MessageStateMachine::transition(message, MessageState::BondPending, message.revision);
    database_.updateMessage(message, message.revision - 1);
    if (!submission.trusted_contact) {
        bonds_.lock(BondRecord{submission.bond_id,
                               message.id,
                               message.sender,
                               policy->owner_address,
                               policy->sink_address,
                               message.policy_hash,
                               policy->bond_amount,
                               message.deadline_unix,
                               std::nullopt});
    }
    MessageStateMachine::transition(message, MessageState::Bonded, message.revision);
    database_.updateMessage(message, message.revision - 1);
    MessageStateMachine::transition(message, MessageState::DeliveryPending, message.revision);
    database_.updateMessage(message, message.revision - 1);
    database_.enqueue("bonded/intake/" + policy->inbox_id,
                      Json{{"message_id", message.id}, {"sender", message.sender}}.dump());
    MessageStateMachine::transition(message, MessageState::PendingReview, message.revision);
    database_.updateMessage(message, message.revision - 1);
    return message;
}

MessageRecord InboxService::finish(MessageRecord message, MessageState decision)
{
    const auto previous = message.revision;
    MessageStateMachine::transition(message, decision, previous);
    message.settlement = MessageStateMachine::requiredSettlement(decision);
    database_.updateMessage(message, previous);

    if (message.bond_amount > 0) {
        const auto settlement = bonds_.settle("bond:" + message.id, *message.settlement);
        database_.enqueue("bonded/receipt/" + message.sender,
                          Json{{"message_id", message.id},
                               {"outcome", toString(settlement.outcome)},
                               {"destination", settlement.destination},
                               {"amount", settlement.amount},
                               {"duplicate", settlement.duplicate}}
                              .dump());
    }
    const auto decision_revision = message.revision;
    MessageStateMachine::transition(message, MessageState::Settled, decision_revision);
    database_.updateMessage(message, decision_revision);
    return message;
}

MessageRecord InboxService::decide(const std::string& message_id, MessageState decision,
                                   bool explicit_owner_action, bool deterministic_violation)
{
    const auto existing = database_.message(message_id);
    if (!existing.has_value()) {
        throw DomainError("unknown message");
    }
    if (decision != MessageState::Accepted && decision != MessageState::Rejected) {
        throw DomainError("owner decision must be accepted or rejected");
    }
    if (decision == MessageState::Rejected && !explicit_owner_action && !deterministic_violation) {
        throw DomainError("classifier output alone cannot reject a bonded message");
    }
    return finish(*existing, decision);
}

MessageRecord InboxService::expire(const std::string& message_id, std::uint64_t now_unix)
{
    const auto existing = database_.message(message_id);
    if (!existing.has_value()) {
        throw DomainError("unknown message");
    }
    if (now_unix < existing->deadline_unix) {
        throw DomainError("message deadline has not passed");
    }
    return finish(*existing, MessageState::Expired);
}

MessageRecord InboxService::deliveryFailed(const std::string& message_id)
{
    const auto existing = database_.message(message_id);
    if (!existing.has_value()) {
        throw DomainError("unknown message");
    }
    return finish(*existing, MessageState::DeliveryFailed);
}

} // namespace bonded
