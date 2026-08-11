#include "services/spending_controller.h"

namespace bonded {

SpendingController::SpendingController(WalletAdapter& wallet, SpendingPolicy policy)
    : wallet_(wallet), policy_(policy)
{
    if (policy_.per_transaction == 0 || policy_.per_period == 0 || policy_.period_seconds == 0 ||
        policy_.approval_timeout_seconds == 0) {
        throw DomainError("spending policy limits and periods must be positive");
    }
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
                                             std::uint64_t now_unix)
{
    if (recipient.empty() || amount == 0) {
        throw DomainError("spending recipient and positive amount are required");
    }
    std::lock_guard lock(mutex_);
    const auto spent = periodSpend(now_unix);
    const auto period_available = spent >= policy_.per_period ? 0 : policy_.per_period - spent;
    if (amount <= policy_.per_transaction && amount <= period_available) {
        const std::string id = "spend-" + std::to_string(++sequence_);
        const auto transfer_id = wallet_.send(recipient, amount, now_unix);
        SpendingProposal executed{id, recipient, amount, now_unix, now_unix,
                                  ApprovalState::Executed, transfer_id};
        proposals_.emplace(id, executed);
        return executed;
    }
    const std::string id = "approval-" + std::to_string(++sequence_);
    SpendingProposal pending{id, recipient, amount, now_unix,
                             now_unix + policy_.approval_timeout_seconds, ApprovalState::Pending, ""};
    proposals_.emplace(id, pending);
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
    proposal.transfer_id = wallet_.send(proposal.recipient, proposal.amount, now_unix);
    proposal.state = ApprovalState::Executed;
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
    return found->second;
}

void SpendingController::expire(std::uint64_t now_unix)
{
    std::lock_guard lock(mutex_);
    for (auto& [id, proposal] : proposals_) {
        (void)id;
        if (proposal.state == ApprovalState::Pending && now_unix > proposal.expires_at) {
            proposal.state = ApprovalState::Expired;
        }
    }
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

} // namespace bonded
