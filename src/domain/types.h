#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bonded {

using Json = nlohmann::json;

enum class MessageState {
    Created,
    BondPending,
    Bonded,
    DeliveryPending,
    PendingReview,
    Accepted,
    Rejected,
    Expired,
    DeliveryFailed,
    Settled,
};

enum class SettlementOutcome { RefundAccepted, SinkRejected, RefundExpired, RefundDeliveryFailed };

enum class Profile { Inbox, Vault, Settlement };

struct AttachmentPolicy {
    std::vector<std::string> allowed_types;
    std::uint64_t max_bytes{0};
    std::uint32_t max_count{1};
};

struct InboxPolicy {
    std::string inbox_id;
    std::string owner_address;
    std::string sink_address;
    std::string emergency_channel;
    std::string network;
    std::uint64_t version{0};
    std::uint64_t bond_amount{0};
    std::uint64_t response_timeout_seconds{0};
    std::uint64_t valid_from_unix{0};
    AttachmentPolicy attachments;
    std::string signer_public_key;
    std::string signature;
};

struct MessageRecord {
    std::string id;
    std::string sender;
    std::string policy_hash;
    std::uint64_t policy_version{0};
    std::uint64_t bond_amount{0};
    std::uint64_t deadline_unix{0};
    MessageState state{MessageState::Created};
    std::uint64_t revision{0};
    std::optional<SettlementOutcome> settlement;
};

struct BondRecord {
    std::string id;
    std::string message_id;
    std::string sender;
    std::string owner;
    std::string sink;
    std::string policy_hash;
    std::uint64_t amount{0};
    std::uint64_t deadline_unix{0};
    std::optional<SettlementOutcome> outcome;
};

struct WalletTransfer {
    std::string id;
    std::string recipient;
    std::uint64_t amount{0};
    std::uint64_t timestamp_unix{0};
};

class DomainError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string toString(MessageState state);
MessageState messageStateFromString(const std::string& value);
std::string toString(SettlementOutcome outcome);
SettlementOutcome settlementOutcomeFromString(const std::string& value);
std::string toString(Profile profile);
Profile profileFromString(const std::string& value);

void to_json(Json& json, const AttachmentPolicy& value);
void from_json(const Json& json, AttachmentPolicy& value);
void to_json(Json& json, const InboxPolicy& value);
void from_json(const Json& json, InboxPolicy& value);
void to_json(Json& json, const MessageRecord& value);
void from_json(const Json& json, MessageRecord& value);
void to_json(Json& json, const BondRecord& value);
void from_json(const Json& json, BondRecord& value);

} // namespace bonded
