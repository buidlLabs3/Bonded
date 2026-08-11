#include "services/contact_rules.h"

namespace bonded {

ContactRules::ContactRules(std::size_t rate_limit, std::uint64_t window_seconds)
    : rate_limit_(rate_limit), window_seconds_(window_seconds)
{
    if (rate_limit_ == 0 || window_seconds_ == 0) {
        throw DomainError("rate limit and window must be positive");
    }
}

void ContactRules::trust(const std::string& authenticated_identity)
{
    if (authenticated_identity.empty()) {
        throw DomainError("trusted identity is required");
    }
    std::lock_guard lock(mutex_);
    trusted_.insert(authenticated_identity);
}

void ContactRules::revoke(const std::string& authenticated_identity)
{
    std::lock_guard lock(mutex_);
    trusted_.erase(authenticated_identity);
}

bool ContactRules::isTrusted(const std::string& authenticated_identity) const
{
    std::lock_guard lock(mutex_);
    return trusted_.contains(authenticated_identity);
}

bool ContactRules::allow(const std::string& sender_commitment, std::uint64_t now_unix)
{
    if (sender_commitment.empty()) {
        throw DomainError("sender commitment is required");
    }
    std::lock_guard lock(mutex_);
    auto& attempts = attempts_[sender_commitment];
    const auto oldest_allowed = now_unix > window_seconds_ ? now_unix - window_seconds_ : 0;
    while (!attempts.empty() && attempts.front() < oldest_allowed) {
        attempts.pop_front();
    }
    if (attempts.size() >= rate_limit_) {
        return false;
    }
    attempts.push_back(now_unix);
    return true;
}

} // namespace bonded
