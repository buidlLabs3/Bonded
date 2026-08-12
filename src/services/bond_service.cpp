#include "services/bond_service.h"

namespace bonded {

BondService::BondService(Database& database) : database_(database) {}

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
    if (database_.createBond(bond)) {
        return bond;
    }
    const auto found = database_.bond(bond.id);
    if (!found.has_value() || Json(*found) != Json(bond)) {
        throw DomainError("conflicting duplicate bond");
    }
    return *found;
}

std::optional<BondRecord> BondService::get(const std::string& bond_id) const
{
    std::lock_guard lock(mutex_);
    return database_.bond(bond_id);
}

SettlementResult BondService::settle(const std::string& bond_id, SettlementOutcome outcome)
{
    std::lock_guard lock(mutex_);
    auto bond = database_.bond(bond_id);
    if (!bond.has_value()) {
        throw DomainError("unknown bond");
    }
    if (bond->outcome.has_value()) {
        if (*bond->outcome != outcome) {
            throw DomainError("conflicting terminal bond settlement");
        }
        const std::string destination =
            outcome == SettlementOutcome::SinkRejected ? bond->sink : bond->sender;
        return {bond->id, destination, bond->amount, outcome, true};
    }
    bond->outcome = outcome;
    const std::string destination =
        outcome == SettlementOutcome::SinkRejected ? bond->sink : bond->sender;
    if (destination == bond->owner) {
        throw DomainError("bond settlement cannot pay the inbox owner");
    }
    if (!database_.settleBond(*bond)) {
        const auto concurrent = database_.bond(bond_id);
        if (!concurrent.has_value() || concurrent->outcome != outcome) {
            throw DomainError("conflicting terminal bond settlement");
        }
        return {bond->id, destination, bond->amount, outcome, true};
    }
    return {bond->id, destination, bond->amount, outcome, false};
}

std::size_t BondService::size() const
{
    std::lock_guard lock(mutex_);
    return database_.bondCount();
}

} // namespace bonded
