#include "runtime/reliability.h"

#include <algorithm>
#include <cctype>

namespace bonded {
namespace {

bool sensitive(std::string key)
{
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char character) { return std::tolower(character); });
    return key.find("content") != std::string::npos || key.find("payload") != std::string::npos ||
           key.find("private") != std::string::npos || key.find("secret") != std::string::npos ||
           key.find("attachment") != std::string::npos || key == "key" || key == "signature";
}

} // namespace

Json RedactedTelemetry::event(const std::string& type, const std::string& correlation_id,
                              const Json& fields)
{
    if (type.empty() || correlation_id.empty() || !fields.is_object()) {
        throw DomainError("telemetry type, correlation id, and object fields are required");
    }
    return Json{{"type", type}, {"correlation_id", correlation_id}, {"fields", redact(fields)}};
}

Json RedactedTelemetry::redact(const Json& input)
{
    if (input.is_array()) {
        Json result = Json::array();
        for (const auto& value : input) {
            result.push_back(redact(value));
        }
        return result;
    }
    if (!input.is_object()) {
        return input;
    }
    Json result = Json::object();
    for (const auto& [key, value] : input.items()) {
        result[key] = sensitive(key) ? Json("[REDACTED]") : redact(value);
    }
    return result;
}

CircuitBreaker::CircuitBreaker(std::size_t failure_threshold,
                               std::uint64_t cooldown_seconds)
    : failure_threshold_(failure_threshold), cooldown_seconds_(cooldown_seconds)
{
    if (failure_threshold_ == 0 || cooldown_seconds_ == 0) {
        throw DomainError("circuit breaker threshold and cooldown must be positive");
    }
}

bool CircuitBreaker::permit(std::uint64_t now_unix) const
{
    std::lock_guard lock(mutex_);
    return consecutive_failures_ < failure_threshold_ ||
           now_unix >= opened_at_ + cooldown_seconds_;
}

void CircuitBreaker::recordSuccess()
{
    std::lock_guard lock(mutex_);
    consecutive_failures_ = 0;
    opened_at_ = 0;
}

void CircuitBreaker::recordFailure(std::uint64_t now_unix)
{
    std::lock_guard lock(mutex_);
    ++consecutive_failures_;
    if (consecutive_failures_ == failure_threshold_) {
        opened_at_ = now_unix;
    }
}

Json CircuitBreaker::status(std::uint64_t now_unix) const
{
    std::lock_guard lock(mutex_);
    const bool open = consecutive_failures_ >= failure_threshold_ &&
                      now_unix < opened_at_ + cooldown_seconds_;
    return Json{{"state", open ? "open" : "closed"},
                {"consecutive_failures", consecutive_failures_}};
}

BoundedQueue::BoundedQueue(std::size_t capacity) : capacity_(capacity)
{
    if (capacity_ == 0) {
        throw DomainError("queue capacity must be positive");
    }
}

void BoundedQueue::push(Json value)
{
    std::lock_guard lock(mutex_);
    if (queue_.size() >= capacity_) {
        throw DomainError("queue capacity exceeded");
    }
    queue_.push_back(std::move(value));
}

Json BoundedQueue::pop()
{
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
        throw DomainError("queue is empty");
    }
    Json result = std::move(queue_.front());
    queue_.pop_front();
    return result;
}

std::size_t BoundedQueue::size() const
{
    std::lock_guard lock(mutex_);
    return queue_.size();
}

} // namespace bonded
