#include "services/reconciler.h"

namespace bonded {

Reconciler::Reconciler(Database& database, BondService& bonds, A2AService& a2a)
    : database_(database), bonds_(bonds), a2a_(a2a)
{
}

RecoverySnapshot Reconciler::inspect(std::size_t outbox_limit) const
{
    if (outbox_limit == 0) {
        throw DomainError("reconciliation outbox limit must be positive");
    }
    return {database_.pendingOutbox(outbox_limit).size(), bonds_.size(), a2a_.activeTaskCount(),
            true};
}

void to_json(Json& json, const RecoverySnapshot& snapshot)
{
    json = Json{{"pending_outbox", snapshot.pending_outbox},
                {"known_bonds", snapshot.known_bonds},
                {"active_tasks", snapshot.active_tasks},
                {"ready", snapshot.ready}};
}

} // namespace bonded
