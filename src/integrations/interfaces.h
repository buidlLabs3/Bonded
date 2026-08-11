#pragma once

#include "domain/types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bonded {

struct StoredObject {
    std::string address;
    std::string label;
    std::uint64_t size{0};
};

struct WalletTransfer {
    std::string id;
    std::string recipient;
    std::uint64_t amount{0};
    std::uint64_t timestamp_unix{0};
};

class MessagingAdapter {
public:
    virtual ~MessagingAdapter() = default;
    virtual std::string send(const std::string& topic, const std::string& payload) = 0;
    virtual void subscribe(const std::string& topic,
                           std::function<void(const std::string&)> handler) = 0;
    virtual void join(const std::string& group_id) = 0;
    virtual std::string createGroup(const std::vector<std::string>& members) = 0;
};

class StorageAdapter {
public:
    virtual ~StorageAdapter() = default;
    virtual std::string put(const std::string& payload) = 0;
    virtual std::string get(const std::string& address) const = 0;
};

class WalletAdapter {
public:
    virtual ~WalletAdapter() = default;
    virtual std::uint64_t balance() const = 0;
    virtual std::string send(const std::string& recipient, std::uint64_t amount,
                             std::uint64_t now_unix) = 0;
    virtual std::vector<WalletTransfer> history() const = 0;
};

class ProgramAdapter {
public:
    virtual ~ProgramAdapter() = default;
    virtual Json query(const std::string& program_id, const Json& parameters) const = 0;
    virtual std::string call(const std::string& program_id, const std::string& instruction,
                             const Json& parameters) = 0;
    virtual std::string deploy(const std::string& binary_path) = 0;
};

} // namespace bonded
