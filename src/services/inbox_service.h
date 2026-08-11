#pragma once

#include "domain/policy.h"
#include "services/bond_service.h"
#include "storage/database.h"

#include <string>

namespace bonded {

struct Submission {
    std::string inbox_id;
    std::string message_id;
    std::string idempotency_key;
    std::string sender;
    std::string policy_hash;
    std::string bond_id;
    std::uint64_t bond_amount{0};
    std::uint64_t now_unix{0};
    bool trusted_contact{false};
    std::uint32_t attachment_count{0};
    std::uint64_t attachment_bytes{0};
    std::string attachment_type;
};

class InboxService {
public:
    InboxService(Database& database, BondService& bonds);

    InboxPolicy publishPolicy(const InboxPolicy& policy);
    MessageRecord submit(const Submission& submission);
    MessageRecord decide(const std::string& message_id, MessageState decision,
                         bool explicit_owner_action, bool deterministic_violation);
    MessageRecord expire(const std::string& message_id, std::uint64_t now_unix);
    MessageRecord deliveryFailed(const std::string& message_id);

private:
    MessageRecord finish(MessageRecord message, MessageState decision);
    Database& database_;
    BondService& bonds_;
};

} // namespace bonded
