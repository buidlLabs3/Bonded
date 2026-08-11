#pragma once

#include "domain/types.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace bonded {

class ConfigurationService {
public:
    ConfigurationService(Json initial_values, std::string owner_public_key);

    Json snapshot() const;
    Json update(const Json& request);
    static std::string signingPayload(const Json& request);

private:
    static void validateChanges(const Json& changes);

    mutable std::mutex mutex_;
    Json values_;
    std::string owner_public_key_;
    std::uint64_t revision_{0};
};

} // namespace bonded
