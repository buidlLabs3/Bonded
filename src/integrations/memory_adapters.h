#pragma once

#include "integrations/interfaces.h"

#include <map>
#include <mutex>

namespace bonded {

class MemoryMessagingAdapter : public MessagingAdapter {
public:
    std::string send(const std::string& topic, const std::string& payload) override;
    void subscribe(const std::string& topic,
                   std::function<void(const std::string&)> handler) override;
    void join(const std::string& group_id) override;
    std::string createGroup(const std::vector<std::string>& members) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::vector<std::function<void(const std::string&)>>> subscribers_;
    std::uint64_t sequence_{0};
};

class MemoryStorageAdapter : public StorageAdapter {
public:
    std::string put(const std::string& payload) override;
    std::string get(const std::string& address) const override;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> objects_;
};

class MemoryWalletAdapter : public WalletAdapter {
public:
    explicit MemoryWalletAdapter(std::uint64_t initial_balance);
    std::uint64_t balance() const override;
    std::string send(const std::string& recipient, std::uint64_t amount,
                     std::uint64_t now_unix) override;
    std::vector<WalletTransfer> history() const override;

private:
    mutable std::mutex mutex_;
    std::uint64_t balance_;
    std::vector<WalletTransfer> history_;
};

class MemoryProgramAdapter : public ProgramAdapter {
public:
    Json query(const std::string& program_id, const Json& parameters) const override;
    std::string call(const std::string& program_id, const std::string& instruction,
                     const Json& parameters) override;
    std::string deploy(const std::string& binary_path) override;

private:
    mutable std::mutex mutex_;
    std::map<std::string, Json> state_;
    std::uint64_t sequence_{0};
};

} // namespace bonded
