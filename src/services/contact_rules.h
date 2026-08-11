#pragma once

#include "domain/types.h"

#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace bonded {

class ContactRules {
public:
    ContactRules(std::size_t rate_limit, std::uint64_t window_seconds);
    void trust(const std::string& authenticated_identity);
    void revoke(const std::string& authenticated_identity);
    bool isTrusted(const std::string& authenticated_identity) const;
    bool allow(const std::string& sender_commitment, std::uint64_t now_unix);

private:
    const std::size_t rate_limit_;
    const std::uint64_t window_seconds_;
    mutable std::mutex mutex_;
    std::set<std::string> trusted_;
    std::map<std::string, std::deque<std::uint64_t>> attempts_;
};

} // namespace bonded
