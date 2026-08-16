#pragma once

#include "domain/types.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace bonded {

class ConfigurationService {
public:
    using Load = std::function<std::optional<Json>()>;
    using Save = std::function<void(const Json&)>;

    ConfigurationService(Json initial_values, std::string owner_public_key,
                         Load load = {}, Save save = {});

    Json snapshot() const;
    Json update(const Json& request);
    static std::string signingPayload(const Json& request);

private:
    static void validateChanges(const Json& changes);

    mutable std::mutex mutex_;
    Json values_;
    std::string owner_public_key_;
    Save save_;
    std::uint64_t revision_{0};
};

} // namespace bonded
