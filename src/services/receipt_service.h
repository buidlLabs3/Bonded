#pragma once

#include "domain/types.h"

#include <string>

namespace bonded {

struct SettlementReceipt {
    std::string protocol;
    std::string network;
    std::string message_id;
    std::string bond_id;
    std::string policy_hash;
    std::string transaction_id;
    std::string destination_commitment;
    std::uint64_t amount{0};
    std::uint64_t settled_at{0};
    SettlementOutcome outcome;
    std::string signer_public_key;
    std::string signature;
};

class ReceiptService {
public:
    static std::string canonicalUnsigned(const SettlementReceipt& receipt);
    static SettlementReceipt sign(SettlementReceipt receipt, const std::string& private_key,
                                  const std::string& public_key);
    static bool verify(const SettlementReceipt& receipt);
};

void to_json(Json& json, const SettlementReceipt& receipt);
void from_json(const Json& json, SettlementReceipt& receipt);

} // namespace bonded
