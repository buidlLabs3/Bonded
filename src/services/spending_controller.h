#pragma once

#include "integrations/interfaces.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace bonded {

enum class ApprovalState { Pending, Approved, Denied, Expired, Executed };

struct SpendingPolicy {
    std::uint64_t per_transaction{0};
    std::uint64_t per_period{0};
    std::uint64_t period_seconds{86400};
    std::uint64_t approval_timeout_seconds{3600};
};

struct SpendingProposal {
    std::string id;
    std::string recipient;
    std::uint64_t amount{0};
    std::uint64_t created_at{0};
    std::uint64_t expires_at{0};
    ApprovalState state{ApprovalState::Pending};
    std::string transfer_id;
};

class SpendingController {
public:
    SpendingController(WalletAdapter& wallet, SpendingPolicy policy);
    SpendingProposal propose(const std::string& recipient, std::uint64_t amount,
                             std::uint64_t now_unix);
    SpendingProposal approve(const std::string& proposal_id, std::uint64_t now_unix);
    SpendingProposal deny(const std::string& proposal_id);
    void expire(std::uint64_t now_unix);
    std::optional<SpendingProposal> get(const std::string& proposal_id) const;

private:
    std::uint64_t periodSpend(std::uint64_t now_unix) const;
    WalletAdapter& wallet_;
    SpendingPolicy policy_;
    mutable std::mutex mutex_;
    std::map<std::string, SpendingProposal> proposals_;
    std::uint64_t sequence_{0};
};

std::string toString(ApprovalState state);

} // namespace bonded
