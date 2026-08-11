#include "services/storage_service.h"

#include <algorithm>

namespace bonded {

StorageService::StorageService(StorageAdapter& adapter) : adapter_(adapter) {}

StorageEntry StorageService::upload(const std::string& plaintext, const std::string& label,
                                    const std::string& key_hex)
{
    if (label.empty()) {
        throw DomainError("storage label is required");
    }
    const auto commitment = Crypto::sha256(plaintext);
    const auto encrypted = Crypto::encryptAes256Gcm(key_hex, plaintext, "storage:" + commitment);
    const auto payload = Json{{"version", 1},
                              {"commitment", commitment},
                              {"nonce", encrypted.nonce_hex},
                              {"ciphertext", encrypted.data_hex},
                              {"tag", encrypted.tag_hex}}
                             .dump();
    const auto address = adapter_.put(payload);
    StorageEntry entry{address, label, plaintext.size(), commitment};
    std::lock_guard lock(mutex_);
    entries_.emplace(address, entry);
    return entry;
}

std::string StorageService::download(const std::string& address, const std::string& key_hex) const
{
    const auto payload = Json::parse(adapter_.get(address));
    const auto commitment = payload.at("commitment").get<std::string>();
    Ciphertext encrypted{payload.at("nonce").get<std::string>(),
                         payload.at("ciphertext").get<std::string>(),
                         payload.at("tag").get<std::string>()};
    const auto plaintext = Crypto::decryptAes256Gcm(key_hex, encrypted, "storage:" + commitment);
    if (Crypto::sha256(plaintext) != commitment) {
        throw DomainError("stored object commitment mismatch");
    }
    return plaintext;
}

std::vector<StorageEntry> StorageService::list() const
{
    std::lock_guard lock(mutex_);
    std::vector<StorageEntry> result;
    result.reserve(entries_.size());
    for (const auto& [address, entry] : entries_) {
        (void)address;
        result.push_back(entry);
    }
    return result;
}

StorageGrant StorageService::share(const std::string& address, const std::string& recipient,
                                   std::uint64_t expires_at, const std::string& wrapped_key)
{
    if (recipient.empty() || wrapped_key.empty() || expires_at == 0) {
        throw DomainError("storage grant recipient, expiry, and wrapped key are required");
    }
    std::lock_guard lock(mutex_);
    if (!entries_.contains(address)) {
        throw DomainError("cannot share unknown stored object");
    }
    StorageGrant grant{address, recipient, expires_at, wrapped_key};
    grants_.push_back(grant);
    return grant;
}

bool StorageService::canAccess(const std::string& address, const std::string& recipient,
                               std::uint64_t now_unix) const
{
    std::lock_guard lock(mutex_);
    return std::any_of(grants_.begin(), grants_.end(), [&](const StorageGrant& grant) {
        return grant.address == address && grant.recipient == recipient &&
               grant.expires_at >= now_unix;
    });
}

void to_json(Json& json, const StorageEntry& entry)
{
    json = Json{{"address", entry.address},
                {"label", entry.label},
                {"plaintext_bytes", entry.plaintext_bytes},
                {"commitment", entry.commitment}};
}

void from_json(const Json& json, StorageEntry& entry)
{
    json.at("address").get_to(entry.address);
    json.at("label").get_to(entry.label);
    json.at("plaintext_bytes").get_to(entry.plaintext_bytes);
    json.at("commitment").get_to(entry.commitment);
}

void to_json(Json& json, const StorageGrant& grant)
{
    json = Json{{"address", grant.address},
                {"recipient", grant.recipient},
                {"expires_at", grant.expires_at},
                {"wrapped_key", grant.wrapped_key}};
}

void from_json(const Json& json, StorageGrant& grant)
{
    json.at("address").get_to(grant.address);
    json.at("recipient").get_to(grant.recipient);
    json.at("expires_at").get_to(grant.expires_at);
    json.at("wrapped_key").get_to(grant.wrapped_key);
}

} // namespace bonded
