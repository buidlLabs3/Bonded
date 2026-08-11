#include "services/bond_service.h"

namespace bonded {

BondRecord BondService::lock(BondRecord bond)
{
    if (bond.id.empty() || bond.message_id.empty() || bond.sender.empty() || bond.owner.empty() ||
        bond.sink.empty() || bond.policy_hash.empty()) {
        throw DomainError("bond identifiers, participants, and policy commitment are required");
    }
    if (bond.owner == bond.sink) {
        throw DomainError("bond sink cannot be the inbox owner");
    }
    if (bond.amount == 0 || bond.deadline_unix == 0) {
        throw DomainError("bond amount and deadline must be positive");
    }
    std::lock_guard lock(mutex_);
    const auto [found, inserted] = bonds_.emplace(bond.id, bond);
    if (!inserted && Json(found->second) != Json(bond)) {
        throw DomainError("conflicting duplicate bond");
    }
    return found->second;
}

std::optional<BondRecord> BondService::get(const std::string& bond_id) const
{
    std::lock_guard lock(mutex_);
    const auto found = bonds_.find(bond_id);
    return found == bonds_.end() ? std::nullopt : std::optional<BondRecord>{found->second};
}

SettlementResult BondService::settle(const std::string& bond_id, SettlementOutcome outcome)
{
    std::lock_guard lock(mutex_);
    const auto found = bonds_.find(bond_id);
    if (found == bonds_.end()) {
        throw DomainError("unknown bond");
    }
    auto& bond = found->second;
    if (bond.outcome.has_value()) {
        if (*bond.outcome != outcome) {
            throw DomainError("conflicting terminal bond settlement");
        }
        const std::string destination = outcome == SettlementOutcome::SinkRejected ? bond.sink
                                                                                    : bond.sender;
        return {bond.id, destination, bond.amount, outcome, true};
    }
    bond.outcome = outcome;
    const std::string destination = outcome == SettlementOutcome::SinkRejected ? bond.sink
                                                                                : bond.sender;
    if (destination == bond.owner) {
        throw DomainError("bond settlement cannot pay the inbox owner");
    }
    return {bond.id, destination, bond.amount, outcome, false};
}

std::size_t BondService::size() const
{
    std::lock_guard lock(mutex_);
    return bonds_.size();
}

} // namespace bonded
