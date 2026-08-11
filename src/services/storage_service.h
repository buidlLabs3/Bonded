#pragma once

#include "integrations/interfaces.h"
#include "security/crypto.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bonded {

struct StorageEntry {
    std::string address;
    std::string label;
    std::uint64_t plaintext_bytes{0};
    std::string commitment;
};

struct StorageGrant {
    std::string address;
    std::string recipient;
    std::uint64_t expires_at{0};
    std::string wrapped_key;
};

class StorageService {
public:
    explicit StorageService(StorageAdapter& adapter);
    StorageEntry upload(const std::string& plaintext, const std::string& label,
                        const std::string& key_hex);
    std::string download(const std::string& address, const std::string& key_hex) const;
    std::vector<StorageEntry> list() const;
    StorageGrant share(const std::string& address, const std::string& recipient,
                       std::uint64_t expires_at, const std::string& wrapped_key);
    bool canAccess(const std::string& address, const std::string& recipient,
                   std::uint64_t now_unix) const;

private:
    StorageAdapter& adapter_;
    mutable std::mutex mutex_;
    std::map<std::string, StorageEntry> entries_;
    std::vector<StorageGrant> grants_;
};

void to_json(Json& json, const StorageEntry& entry);
void from_json(const Json& json, StorageEntry& entry);
void to_json(Json& json, const StorageGrant& grant);
void from_json(const Json& json, StorageGrant& grant);

} // namespace bonded
