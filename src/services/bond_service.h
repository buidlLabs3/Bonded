#pragma once

#include "domain/types.h"
#include "storage/database.h"

#include <mutex>
#include <optional>
#include <string>

namespace bonded {

struct SettlementResult {
    std::string bond_id;
    std::string destination;
    std::uint64_t amount{0};
    SettlementOutcome outcome;
    bool duplicate{false};
};

class BondService {
public:
    explicit BondService(Database& database);
    BondRecord lock(BondRecord bond);
    std::optional<BondRecord> get(const std::string& bond_id) const;
    SettlementResult settle(const std::string& bond_id, SettlementOutcome outcome);
    std::size_t size() const;

private:
    Database& database_;
    mutable std::mutex mutex_;
};

} // namespace bonded
