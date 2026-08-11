#include "domain/policy.h"

#include "security/crypto.h"

#include <algorithm>

namespace bonded {

void PolicyService::validate(const InboxPolicy& policy)
{
    if (policy.inbox_id.empty() || policy.owner_address.empty() || policy.sink_address.empty() ||
        policy.network.empty()) {
        throw DomainError("policy identity, owner, sink, and network are required");
    }
    if (policy.owner_address == policy.sink_address) {
        throw DomainError("policy sink cannot be the inbox owner");
    }
    if (policy.emergency_channel.empty()) {
        throw DomainError("bond-free emergency channel is required");
    }
    if (policy.version == 0 || policy.response_timeout_seconds == 0) {
        throw DomainError("policy version and response timeout must be positive");
    }
    if (policy.attachments.max_count > 1) {
        throw DomainError("MVP permits at most one attachment");
    }
    if (policy.attachments.max_count == 1 &&
        (policy.attachments.max_bytes == 0 || policy.attachments.allowed_types.empty())) {
        throw DomainError("attachment size and allowed types are required");
    }
    if (std::any_of(policy.attachments.allowed_types.begin(), policy.attachments.allowed_types.end(),
                    [](const std::string& type) { return type.empty() || type.size() > 127; })) {
        throw DomainError("invalid attachment media type");
    }
}

std::string PolicyService::canonicalUnsigned(const InboxPolicy& policy)
{
    Json json = policy;
    json.erase("signature");
    json.erase("signer_public_key");
    json["protocol"] = "bonded-inbox/policy/v1";
    return json.dump();
}

std::string PolicyService::hash(const InboxPolicy& policy)
{
    return Crypto::sha256(canonicalUnsigned(policy));
}

InboxPolicy PolicyService::sign(InboxPolicy policy, const std::string& private_key_hex,
                                const std::string& public_key_hex)
{
    validate(policy);
    policy.signer_public_key = public_key_hex;
    policy.signature = Crypto::signEd25519(private_key_hex, canonicalUnsigned(policy));
    return policy;
}

bool PolicyService::verify(const InboxPolicy& policy)
{
    try {
        validate(policy);
        return !policy.signer_public_key.empty() && !policy.signature.empty() &&
               Crypto::verifyEd25519(policy.signer_public_key, canonicalUnsigned(policy),
                                     policy.signature);
    } catch (...) {
        return false;
    }
}

void PolicyService::requireSuccessor(const InboxPolicy& current, const InboxPolicy& successor)
{
    validate(successor);
    if (successor.inbox_id != current.inbox_id || successor.owner_address != current.owner_address ||
        successor.network != current.network) {
        throw DomainError("policy successor cannot change inbox, owner, or network identity");
    }
    if (successor.version != current.version + 1) {
        throw DomainError("policy successor version must increment by one");
    }
    if (successor.valid_from_unix < current.valid_from_unix) {
        throw DomainError("policy successor cannot move validity backwards");
    }
}

} // namespace bonded
