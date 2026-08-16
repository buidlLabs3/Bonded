#pragma once

#include "integrations/interfaces.h"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
    Json recipient_private_keys{Json::object()};
    std::uint64_t amount{0};
    std::uint64_t created_at{0};
    std::uint64_t expires_at{0};
    ApprovalState state{ApprovalState::Pending};
    std::string transfer_id;
};

class SpendingController {
public:
    using Load = std::function<std::vector<SpendingProposal>()>;
    using Save = std::function<void(const SpendingProposal&)>;

    SpendingController(WalletAdapter& wallet, SpendingPolicy policy, Load load = {},
                       Save save = {});
    SpendingProposal propose(const std::string& recipient, std::uint64_t amount,
                             std::uint64_t now_unix, const std::string& request_id = "");
    SpendingProposal proposePrivate(const std::string& recipient,
                                    const Json& recipient_private_keys,
                                    std::uint64_t amount, std::uint64_t now_unix,
                                    const std::string& request_id = "");
    SpendingProposal approve(const std::string& proposal_id, std::uint64_t now_unix);
    SpendingProposal deny(const std::string& proposal_id);
    void expire(std::uint64_t now_unix);
    std::optional<SpendingProposal> get(const std::string& proposal_id) const;
    std::vector<SpendingProposal> list() const;

private:
    SpendingProposal proposeImpl(const std::string& recipient,
                                 const Json& recipient_private_keys,
                                 std::uint64_t amount, std::uint64_t now_unix,
                                 const std::string& request_id);
    std::uint64_t periodSpend(std::uint64_t now_unix) const;
    WalletAdapter& wallet_;
    SpendingPolicy policy_;
    mutable std::mutex mutex_;
    std::map<std::string, SpendingProposal> proposals_;
    Save save_;
    std::uint64_t sequence_{0};
};

std::string toString(ApprovalState state);
void to_json(Json& json, const SpendingProposal& proposal);
void from_json(const Json& json, SpendingProposal& proposal);

} // namespace bonded
