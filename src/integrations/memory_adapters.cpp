#include "integrations/memory_adapters.h"

#include "security/crypto.h"

#include <filesystem>

namespace bonded {

std::string MemoryMessagingAdapter::send(const std::string& topic, const std::string& payload)
{
    std::vector<std::function<void(const std::string&)>> handlers;
    std::string id;
    {
        std::lock_guard lock(mutex_);
        id = "memory-message-" + std::to_string(++sequence_);
        handlers = subscribers_[topic];
    }
    for (const auto& handler : handlers) {
        handler(payload);
    }
    return id;
}

void MemoryMessagingAdapter::subscribe(const std::string& topic,
                                       std::function<void(const std::string&)> handler)
{
    if (topic.empty() || !handler) {
        throw DomainError("messaging topic and handler are required");
    }
    std::lock_guard lock(mutex_);
    subscribers_[topic].push_back(std::move(handler));
}

void MemoryMessagingAdapter::join(const std::string& group_id)
{
    if (group_id.empty()) {
        throw DomainError("group id is required");
    }
}

std::string MemoryMessagingAdapter::createGroup(const std::vector<std::string>& members)
{
    if (members.empty()) {
        throw DomainError("group requires at least one member");
    }
    std::lock_guard lock(mutex_);
    return "memory-group-" + std::to_string(++sequence_);
}

std::string MemoryStorageAdapter::put(const std::string& payload)
{
    const std::string address = "sha256:" + Crypto::sha256(payload);
    std::lock_guard lock(mutex_);
    objects_.emplace(address, payload);
    return address;
}

std::string MemoryStorageAdapter::get(const std::string& address) const
{
    std::lock_guard lock(mutex_);
    const auto found = objects_.find(address);
    if (found == objects_.end()) {
        throw DomainError("stored object not found");
    }
    return found->second;
}

MemoryWalletAdapter::MemoryWalletAdapter(std::uint64_t initial_balance) : balance_(initial_balance) {}

std::uint64_t MemoryWalletAdapter::balance() const
{
    std::lock_guard lock(mutex_);
    return balance_;
}

std::string MemoryWalletAdapter::send(const std::string& recipient, std::uint64_t amount,
                                      std::uint64_t now_unix)
{
    if (recipient.empty() || amount == 0) {
        throw DomainError("wallet recipient and positive amount are required");
    }
    std::lock_guard lock(mutex_);
    if (amount > balance_) {
        throw DomainError("insufficient wallet balance");
    }
    balance_ -= amount;
    const std::string id = "memory-transfer-" + std::to_string(history_.size() + 1);
    history_.push_back({id, recipient, amount, now_unix});
    return id;
}

std::vector<WalletTransfer> MemoryWalletAdapter::history() const
{
    std::lock_guard lock(mutex_);
    return history_;
}

Json MemoryProgramAdapter::query(const std::string& program_id, const Json& parameters) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.find(program_id);
    return Json{{"program_id", program_id},
                {"parameters", parameters},
                {"state", found == state_.end() ? Json::object() : found->second}};
}

std::string MemoryProgramAdapter::call(const std::string& program_id,
                                       const std::string& instruction, const Json& parameters)
{
    if (program_id.empty() || instruction.empty()) {
        throw DomainError("program id and instruction are required");
    }
    std::lock_guard lock(mutex_);
    state_[program_id][instruction] = parameters;
    return "memory-program-call-" + std::to_string(++sequence_);
}

std::string MemoryProgramAdapter::deploy(const std::string& binary_path)
{
    if (binary_path.empty() || !std::filesystem::exists(binary_path)) {
        throw DomainError("program binary does not exist");
    }
    std::lock_guard lock(mutex_);
    const std::string id = "memory-program-" + std::to_string(++sequence_);
    state_[id] = Json{{"binary", binary_path}};
    return id;
}

} // namespace bonded
