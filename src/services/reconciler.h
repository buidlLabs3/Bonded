#pragma once

#include "services/a2a_service.h"
#include "services/bond_service.h"
#include "storage/database.h"

#include <cstddef>

namespace bonded {

struct RecoverySnapshot {
    std::size_t pending_outbox{0};
    std::size_t known_bonds{0};
    std::size_t active_tasks{0};
    bool ready{false};
};

class Reconciler {
public:
    Reconciler(Database& database, BondService& bonds, A2AService& a2a);
    RecoverySnapshot inspect(std::size_t outbox_limit = 100) const;

private:
    Database& database_;
    BondService& bonds_;
    A2AService& a2a_;
};

void to_json(Json& json, const RecoverySnapshot& snapshot);

} // namespace bonded
