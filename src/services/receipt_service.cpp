#include "services/receipt_service.h"

#include "security/crypto.h"

namespace bonded {

std::string ReceiptService::canonicalUnsigned(const SettlementReceipt& receipt)
{
    Json json = receipt;
    json.erase("signer_public_key");
    json.erase("signature");
    return json.dump();
}

SettlementReceipt ReceiptService::sign(SettlementReceipt receipt, const std::string& private_key,
                                       const std::string& public_key)
{
    if (receipt.protocol != "bonded-inbox/receipt/v1" || receipt.network.empty() ||
        receipt.message_id.empty() || receipt.bond_id.empty() || receipt.policy_hash.empty() ||
        receipt.transaction_id.empty() || receipt.destination_commitment.empty() ||
        receipt.amount == 0 || receipt.settled_at == 0) {
        throw DomainError("incomplete settlement receipt");
    }
    receipt.signer_public_key = public_key;
    receipt.signature = Crypto::signEd25519(private_key, canonicalUnsigned(receipt));
    return receipt;
}

bool ReceiptService::verify(const SettlementReceipt& receipt)
{
    return receipt.protocol == "bonded-inbox/receipt/v1" && !receipt.network.empty() &&
           !receipt.message_id.empty() && !receipt.bond_id.empty() &&
           !receipt.policy_hash.empty() && !receipt.transaction_id.empty() &&
           !receipt.destination_commitment.empty() && receipt.amount > 0 &&
           receipt.settled_at > 0 && !receipt.signer_public_key.empty() &&
           Crypto::verifyEd25519(receipt.signer_public_key, canonicalUnsigned(receipt),
                                 receipt.signature);
}

void to_json(Json& json, const SettlementReceipt& receipt)
{
    json = Json{{"protocol", receipt.protocol},
                {"network", receipt.network},
                {"message_id", receipt.message_id},
                {"bond_id", receipt.bond_id},
                {"policy_hash", receipt.policy_hash},
                {"transaction_id", receipt.transaction_id},
                {"destination_commitment", receipt.destination_commitment},
                {"amount", receipt.amount},
                {"settled_at", receipt.settled_at},
                {"outcome", toString(receipt.outcome)},
                {"signer_public_key", receipt.signer_public_key},
                {"signature", receipt.signature}};
}

void from_json(const Json& json, SettlementReceipt& receipt)
{
    json.at("protocol").get_to(receipt.protocol);
    json.at("network").get_to(receipt.network);
    json.at("message_id").get_to(receipt.message_id);
    json.at("bond_id").get_to(receipt.bond_id);
    json.at("policy_hash").get_to(receipt.policy_hash);
    json.at("transaction_id").get_to(receipt.transaction_id);
    json.at("destination_commitment").get_to(receipt.destination_commitment);
    json.at("amount").get_to(receipt.amount);
    json.at("settled_at").get_to(receipt.settled_at);
    receipt.outcome = settlementOutcomeFromString(json.at("outcome").get<std::string>());
    json.at("signer_public_key").get_to(receipt.signer_public_key);
    json.at("signature").get_to(receipt.signature);
}

} // namespace bonded
