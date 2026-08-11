#pragma once

#include "domain/types.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace bonded {

class RedactedTelemetry {
public:
    static Json event(const std::string& type, const std::string& correlation_id,
                      const Json& fields);
    static Json redact(const Json& input);
};

class CircuitBreaker {
public:
    CircuitBreaker(std::size_t failure_threshold, std::uint64_t cooldown_seconds);
    bool permit(std::uint64_t now_unix) const;
    void recordSuccess();
    void recordFailure(std::uint64_t now_unix);
    Json status(std::uint64_t now_unix) const;

private:
    const std::size_t failure_threshold_;
    const std::uint64_t cooldown_seconds_;
    mutable std::mutex mutex_;
    std::size_t consecutive_failures_{0};
    std::uint64_t opened_at_{0};
};

class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity);
    void push(Json value);
    Json pop();
    std::size_t size() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<Json> queue_;
};

} // namespace bonded
