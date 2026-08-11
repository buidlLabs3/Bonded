#pragma once

#include "domain/types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace bonded {

struct OutboxRecord {
    std::int64_t id{0};
    std::string topic;
    std::string payload;
    std::uint32_t attempts{0};
};

class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void migrate();
    void savePolicy(const InboxPolicy& policy);
    std::optional<InboxPolicy> latestPolicy(const std::string& inbox_id) const;
    bool createMessage(const MessageRecord& message, const std::string& idempotency_key);
    std::optional<MessageRecord> message(const std::string& message_id) const;
    void updateMessage(const MessageRecord& message, std::uint64_t previous_revision);
    void enqueue(const std::string& topic, const std::string& payload);
    std::vector<OutboxRecord> pendingOutbox(std::size_t limit) const;
    void acknowledgeOutbox(std::int64_t id);
    void recordProcessedEvent(const std::string& event_id);
    bool hasProcessedEvent(const std::string& event_id) const;

private:
    void execute(const std::string& sql) const;
    sqlite3* db_{nullptr};
};

} // namespace bonded
