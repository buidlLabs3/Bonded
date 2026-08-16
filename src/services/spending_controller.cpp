#include "services/spending_controller.h"

#include <limits>

namespace bonded {

namespace {

ApprovalState approvalState(const std::string& state)
{
    if (state == "pending") return ApprovalState::Pending;
    if (state == "approved") return ApprovalState::Approved;
    if (state == "denied") return ApprovalState::Denied;
    if (state == "expired") return ApprovalState::Expired;
    if (state == "executed") return ApprovalState::Executed;
    throw DomainError("unknown persisted approval state");
}

std::uint64_t approvalExpiry(std::uint64_t now_unix, std::uint64_t timeout)
{
    if (now_unix > std::numeric_limits<std::uint64_t>::max() - timeout) {
        throw DomainError("spending approval expiry overflow");
    }
    return now_unix + timeout;
}

} // namespace

SpendingController::SpendingController(WalletAdapter& wallet, SpendingPolicy policy, Load load,
                                       Save save)
    : wallet_(wallet), policy_(policy), save_(std::move(save))
{
    if (policy_.per_transaction == 0 || policy_.per_period == 0 || policy_.period_seconds == 0 ||
        policy_.approval_timeout_seconds == 0) {
        throw DomainError("spending policy limits and periods must be positive");
    }
    if (load) {
        for (auto& proposal : load()) {
            if (proposal.id.empty() || !proposals_.emplace(proposal.id, proposal).second) {
                throw DomainError("persisted spending proposal is invalid or duplicated");
            }
        }
    }
    sequence_ = proposals_.size();
}

std::uint64_t SpendingController::periodSpend(std::uint64_t now_unix) const
{
    const auto start = now_unix > policy_.period_seconds ? now_unix - policy_.period_seconds : 0;
    std::uint64_t total = 0;
    for (const auto& transfer : wallet_.history()) {
        if (transfer.timestamp_unix >= start && transfer.timestamp_unix <= now_unix) {
            if (UINT64_MAX - total < transfer.amount) {
                throw DomainError("period spend overflow");
            }
            total += transfer.amount;
        }
    }
    return total;
}

SpendingProposal SpendingController::propose(const std::string& recipient, std::uint64_t amount,
                                             std::uint64_t now_unix,
                                             const std::string& request_id)
{
    return proposeImpl(recipient, Json::object(), amount, now_unix, request_id);
}

SpendingProposal SpendingController::proposePrivate(
    const std::string& recipient, const Json& recipient_private_keys,
    std::uint64_t amount, std::uint64_t now_unix, const std::string& request_id)
{
    return proposeImpl(recipient, recipient_private_keys, amount, now_unix, request_id);
}

SpendingProposal SpendingController::proposeImpl(
    const std::string& recipient, const Json& recipient_private_keys,
    std::uint64_t amount, std::uint64_t now_unix, const std::string& request_id)
{
    if (recipient.empty() || amount == 0 || !recipient_private_keys.is_object()) {
        throw DomainError("spending recipient and positive amount are required");
    }
    std::lock_guard lock(mutex_);
    const auto requested_id = request_id.empty() ? std::string{} : "spend:" + request_id;
    if (!requested_id.empty()) {
        const auto existing = proposals_.find(requested_id);
        if (existing != proposals_.end()) {
            if (existing->second.recipient != recipient ||
                existing->second.recipient_private_keys != recipient_private_keys ||
                existing->second.amount != amount) {
                throw DomainError("spending request id was reused with different parameters");
            }
            return existing->second;
        }
    }
    const auto spent = periodSpend(now_unix);
    const auto period_available = spent >= policy_.per_period ? 0 : policy_.per_period - spent;
    if (amount <= policy_.per_transaction && amount <= period_available) {
        const std::string id = requested_id.empty() ? "spend-" + std::to_string(++sequence_)
                                                    : requested_id;
        SpendingProposal proposal{id, recipient, recipient_private_keys, amount, now_unix, now_unix,
                                  ApprovalState::Approved, ""};
        proposals_.emplace(id, proposal);
        if (save_) save_(proposal);
        proposal.transfer_id = recipient_private_keys.empty()
                                   ? wallet_.send(recipient, amount, now_unix)
                                   : wallet_.sendPrivate(recipient, recipient_private_keys,
                                                         amount, now_unix);
        proposal.state = ApprovalState::Executed;
        proposals_.at(id) = proposal;
        if (save_) save_(proposal);
        return proposal;
    }
    const std::string id = requested_id.empty() ? "approval-" + std::to_string(++sequence_)
                                                : requested_id;
    SpendingProposal pending{id, recipient, recipient_private_keys, amount, now_unix,
                             approvalExpiry(now_unix, policy_.approval_timeout_seconds),
                             ApprovalState::Pending, ""};
    proposals_.emplace(id, pending);
    if (save_) save_(pending);
    return pending;
}

SpendingProposal SpendingController::approve(const std::string& proposal_id,
                                             std::uint64_t now_unix)
{
    std::lock_guard lock(mutex_);
    auto found = proposals_.find(proposal_id);
    if (found == proposals_.end()) {
        throw DomainError("unknown spending proposal");
    }
    auto& proposal = found->second;
    if (proposal.state == ApprovalState::Executed) {
        return proposal;
    }
    if (proposal.state != ApprovalState::Pending || now_unix > proposal.expires_at) {
        if (proposal.state == ApprovalState::Pending) {
            proposal.state = ApprovalState::Expired;
        }
        throw DomainError("spending proposal is not approvable");
    }
    proposal.state = ApprovalState::Approved;
    if (save_) save_(proposal);
    proposal.transfer_id = proposal.recipient_private_keys.empty()
                               ? wallet_.send(proposal.recipient, proposal.amount, now_unix)
                               : wallet_.sendPrivate(proposal.recipient,
                                                     proposal.recipient_private_keys,
                                                     proposal.amount, now_unix);
    proposal.state = ApprovalState::Executed;
    if (save_) save_(proposal);
    return proposal;
}

SpendingProposal SpendingController::deny(const std::string& proposal_id)
{
    std::lock_guard lock(mutex_);
    auto found = proposals_.find(proposal_id);
    if (found == proposals_.end() || found->second.state != ApprovalState::Pending) {
        throw DomainError("spending proposal is not deniable");
    }
    found->second.state = ApprovalState::Denied;
    if (save_) save_(found->second);
    return found->second;
}

void SpendingController::expire(std::uint64_t now_unix)
{
    std::lock_guard lock(mutex_);
    for (auto& [id, proposal] : proposals_) {
        (void)id;
        if (proposal.state == ApprovalState::Pending && now_unix > proposal.expires_at) {
            proposal.state = ApprovalState::Expired;
            if (save_) save_(proposal);
        }
    }
}

std::vector<SpendingProposal> SpendingController::list() const
{
    std::lock_guard lock(mutex_);
    std::vector<SpendingProposal> result;
    result.reserve(proposals_.size());
    for (const auto& [id, proposal] : proposals_) {
        (void)id;
        result.push_back(proposal);
    }
    return result;
}

std::optional<SpendingProposal> SpendingController::get(const std::string& proposal_id) const
{
    std::lock_guard lock(mutex_);
    const auto found = proposals_.find(proposal_id);
    return found == proposals_.end() ? std::nullopt : std::optional<SpendingProposal>{found->second};
}

std::string toString(ApprovalState state)
{
    switch (state) {
    case ApprovalState::Pending:
        return "pending";
    case ApprovalState::Approved:
        return "approved";
    case ApprovalState::Denied:
        return "denied";
    case ApprovalState::Expired:
        return "expired";
    case ApprovalState::Executed:
        return "executed";
    }
    throw DomainError("unknown approval state");
}

void to_json(Json& json, const SpendingProposal& proposal)
{
    json = Json{{"id", proposal.id},
                {"recipient", proposal.recipient},
                {"recipient_private_keys", proposal.recipient_private_keys},
                {"amount", proposal.amount},
                {"created_at", proposal.created_at},
                {"expires_at", proposal.expires_at},
                {"state", toString(proposal.state)},
                {"transfer_id", proposal.transfer_id}};
}

void from_json(const Json& json, SpendingProposal& proposal)
{
    json.at("id").get_to(proposal.id);
    json.at("recipient").get_to(proposal.recipient);
    proposal.recipient_private_keys = json.value("recipient_private_keys", Json::object());
    json.at("amount").get_to(proposal.amount);
    json.at("created_at").get_to(proposal.created_at);
    json.at("expires_at").get_to(proposal.expires_at);
    proposal.state = approvalState(json.at("state").get<std::string>());
    proposal.transfer_id = json.value("transfer_id", "");
}

} // namespace bonded
