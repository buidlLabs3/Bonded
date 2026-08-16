#include "services/configuration_service.h"

#include "security/crypto.h"

#include <limits>
#include <set>

namespace bonded {

ConfigurationService::ConfigurationService(Json initial_values, std::string owner_public_key,
                                           Load load, Save save)
    : values_(std::move(initial_values)), owner_public_key_(std::move(owner_public_key)),
      save_(std::move(save))
{
    if (!values_.is_object()) {
        throw DomainError("runtime configuration must be an object");
    }
    if (load) {
        const auto persisted = load();
        if (persisted.has_value()) {
            if (!persisted->is_object() || !persisted->contains("revision") ||
                !persisted->contains("values") || !persisted->at("values").is_object()) {
                throw DomainError("persisted runtime configuration is invalid");
            }
            revision_ = persisted->at("revision").get<std::uint64_t>();
            values_ = persisted->at("values");
            validateChanges(values_);
        }
    }
}

Json ConfigurationService::snapshot() const
{
    std::lock_guard lock(mutex_);
    return Json{{"revision", revision_}, {"values", values_}};
}

std::string ConfigurationService::signingPayload(const Json& request)
{
    Json unsigned_request = request;
    unsigned_request.erase("signature");
    return Json{{"protocol", "bonded-inbox/configure/v1"}, {"request", unsigned_request}}.dump();
}

void ConfigurationService::validateChanges(const Json& changes)
{
    static const std::set<std::string> allowed{"classifier_enabled", "rate_limit",
                                                "owner_notifications", "approval_timeout_seconds"};
    if (!changes.is_object() || changes.empty()) {
        throw DomainError("configuration changes must be a non-empty object");
    }
    for (const auto& [key, value] : changes.items()) {
        if (!allowed.contains(key)) {
            throw DomainError("configuration key is not mutable: " + key);
        }
        if ((key == "classifier_enabled" || key == "owner_notifications") &&
            !value.is_boolean()) {
            throw DomainError("configuration boolean has invalid type");
        }
        if (key == "rate_limit" || key == "approval_timeout_seconds") {
            const bool positive =
                (value.is_number_unsigned() && value.get<std::uint64_t>() > 0) ||
                (value.is_number_integer() && !value.is_number_unsigned() &&
                 value.get<std::int64_t>() > 0);
            if (!positive) {
                throw DomainError("configuration numeric limit must be positive");
            }
        }
    }
}

Json ConfigurationService::update(const Json& request)
{
    if (owner_public_key_.empty()) {
        throw DomainError("owner configuration key is not provisioned");
    }
    const auto signature = request.at("signature").get<std::string>();
    if (!Crypto::verifyEd25519(owner_public_key_, signingPayload(request), signature)) {
        throw DomainError("configuration owner signature is invalid");
    }
    const auto expected_revision = request.at("expected_revision").get<std::uint64_t>();
    const auto& changes = request.at("changes");
    validateChanges(changes);

    std::lock_guard lock(mutex_);
    if (expected_revision != revision_) {
        throw DomainError("configuration revision conflict");
    }
    Json updated = values_;
    for (const auto& [key, value] : changes.items()) {
        updated[key] = value;
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw DomainError("configuration revision overflow");
    }
    const auto next_revision = revision_ + 1;
    const Json snapshot{{"revision", next_revision}, {"values", updated}};
    if (save_) save_(snapshot);
    values_ = std::move(updated);
    revision_ = next_revision;
    return Json{{"revision", revision_}, {"values", values_}};
}

} // namespace bonded
