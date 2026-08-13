#include "integrations/logos_adapters.h"

#include "security/crypto.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <string_view>

namespace bonded {
namespace {

constexpr std::size_t maximum_expired_sessions = 1024;
constexpr std::size_t maximum_pending_sessions = 1024;
constexpr std::size_t maximum_download_bytes = 4 * 1024 * 1024;
constexpr std::size_t maximum_encoded_chunk_bytes =
    ((maximum_download_bytes + 2) / 3) * 4;

void rememberExpired(std::set<std::string>& sessions, const std::string& session)
{
    sessions.insert(session);
    while (sessions.size() > maximum_expired_sessions) {
        sessions.erase(sessions.begin());
    }
}

template <typename Pending>
void discardUnclaimed(Pending& pending, const std::set<std::string>& waiting,
                      std::size_t starting)
{
    if (starting != 0) {
        return;
    }
    std::erase_if(pending, [&](const auto& item) { return !waiting.contains(item.first); });
}

std::string requireString(const Json& value, const char* field)
{
    if (!value.contains(field) || !value.at(field).is_string()) {
        throw DomainError(std::string("Logos event omitted ") + field);
    }
    return value.at(field).get<std::string>();
}

Json parseEvent(const std::string& event_json)
{
    try {
        const auto event = Json::parse(event_json);
        if (!event.is_object()) {
            throw DomainError("Logos Storage event must be a JSON object");
        }
        return event;
    } catch (const DomainError&) {
        throw;
    } catch (const std::exception&) {
        throw DomainError("Logos Storage returned invalid event JSON");
    }
}

bool optionalBoolean(const Json& event, const char* field, bool fallback)
{
    if (!event.contains(field)) {
        return fallback;
    }
    if (!event.at(field).is_boolean()) {
        throw DomainError(std::string("Logos Storage event has invalid ") + field);
    }
    return event.at(field).get<bool>();
}

std::string optionalString(const Json& event, const char* field,
                           const std::string& fallback)
{
    if (!event.contains(field)) {
        return fallback;
    }
    if (!event.at(field).is_string()) {
        throw DomainError(std::string("Logos Storage event has invalid ") + field);
    }
    return event.at(field).get<std::string>();
}

std::string decodeBase64(const std::string& encoded)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> decode{};
    decode.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        decode[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }

    std::string compact;
    std::copy_if(encoded.begin(), encoded.end(), std::back_inserter(compact), [](const auto byte) {
        return !std::isspace(static_cast<unsigned char>(byte));
    });
    if (compact.empty() || compact.size() % 4 != 0) {
        throw DomainError("Logos Storage returned invalid base64");
    }
    const auto padding = compact.ends_with("==") ? 2U : compact.ends_with("=") ? 1U : 0U;
    if (compact.find('=') < compact.size() - padding) {
        throw DomainError("Logos Storage returned invalid base64");
    }
    const auto encoded_bytes = compact.size() - padding;
    for (const auto byte : compact.substr(0, encoded_bytes)) {
        if (decode[static_cast<unsigned char>(byte)] < 0) {
            throw DomainError("Logos Storage returned invalid base64");
        }
    }
    if ((padding == 1 &&
         (decode[static_cast<unsigned char>(compact[encoded_bytes - 1])] & 0x03) != 0) ||
        (padding == 2 &&
         (decode[static_cast<unsigned char>(compact[encoded_bytes - 1])] & 0x0f) != 0)) {
        throw DomainError("Logos Storage returned non-canonical base64");
    }

    std::string output;
    int bits = 0;
    unsigned value = 0;
    for (const auto byte : compact.substr(0, encoded_bytes)) {
        const auto character = static_cast<unsigned char>(byte);
        value = (value << 6U) | static_cast<unsigned>(decode[character]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((value >> bits) & 0xffU));
        }
    }
    return output;
}

} // namespace

LogosMessagingAdapter::LogosMessagingAdapter(Send send, Subscribe subscribe)
    : send_(std::move(send)), subscribe_(std::move(subscribe))
{
    if (!send_ || !subscribe_) {
        throw DomainError("Logos Messaging callbacks are required");
    }
}

std::string LogosMessagingAdapter::send(const std::string& topic, const std::string& payload)
{
    if (topic.empty()) {
        throw DomainError("Logos Messaging topic is required");
    }
    const auto request_id = send_(topic, payload);
    if (request_id.empty()) {
        throw DomainError("Logos Delivery returned an empty request id");
    }
    return request_id;
}

void LogosMessagingAdapter::subscribe(const std::string& topic,
                                      std::function<void(const std::string&)> handler)
{
    if (topic.empty() || !handler) {
        throw DomainError("Logos Messaging subscription is invalid");
    }
    ensureSubscribed(topic, std::move(handler));
}

void LogosMessagingAdapter::ensureSubscribed(
    const std::string& topic, std::function<void(const std::string&)> handler)
{
    std::unique_lock lock(mutex_);
    const auto subscriber_id = handler ? ++next_subscriber_id_ : 0;
    if (handler) {
        subscribers_[topic].push_back(Subscriber{subscriber_id, std::move(handler)});
    }
    while (!subscribed_topics_.contains(topic)) {
        if (subscribing_topics_.insert(topic).second) {
            lock.unlock();
            try {
                subscribe_(topic);
            } catch (...) {
                lock.lock();
                if (subscriber_id != 0) {
                    auto& handlers = subscribers_[topic];
                    std::erase_if(handlers, [&](const auto& item) {
                        return item.id == subscriber_id;
                    });
                    if (handlers.empty()) {
                        subscribers_.erase(topic);
                    }
                }
                subscribing_topics_.erase(topic);
                subscription_changed_.notify_all();
                throw;
            }
            lock.lock();
            subscribed_topics_.insert(topic);
            subscribing_topics_.erase(topic);
            subscription_changed_.notify_all();
            return;
        }
        subscription_changed_.wait(lock, [&] {
            return subscribed_topics_.contains(topic) ||
                   !subscribing_topics_.contains(topic);
        });
    }
}

std::string LogosMessagingAdapter::groupTopic(const std::string& group_id)
{
    if (group_id.empty()) {
        throw DomainError("messaging group id is required");
    }
    return "bonded/group/" + group_id;
}

void LogosMessagingAdapter::join(const std::string& group_id)
{
    ensureSubscribed(groupTopic(group_id));
}

std::string LogosMessagingAdapter::createGroup(const std::vector<std::string>& members)
{
    if (members.empty() ||
        std::any_of(members.begin(), members.end(), [](const auto& member) {
            return member.empty();
        })) {
        throw DomainError("messaging group requires non-empty members");
    }
    auto canonical = members;
    std::sort(canonical.begin(), canonical.end());
    canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());
    std::string material;
    for (const auto& member : canonical) {
        material += std::to_string(member.size()) + ":" + member;
    }
    const auto group_id = Crypto::sha256(material).substr(0, 32);
    join(group_id);
    return group_id;
}

void LogosMessagingAdapter::receive(const std::string& topic, const std::string& payload)
{
    std::vector<std::function<void(const std::string&)>> handlers;
    {
        std::lock_guard lock(mutex_);
        const auto found = subscribers_.find(topic);
        if (found != subscribers_.end()) {
            for (const auto& subscriber : found->second) {
                handlers.push_back(subscriber.handler);
            }
        }
    }
    std::exception_ptr first_failure;
    for (const auto& handler : handlers) {
        try {
            handler(payload);
        } catch (...) {
            if (!first_failure) {
                first_failure = std::current_exception();
            }
        }
    }
    if (first_failure) {
        std::rethrow_exception(first_failure);
    }
}

LogosStorageAdapter::LogosStorageAdapter(Upload upload, Download download,
                                         Cancel cancel_upload, Cancel cancel_download,
                                         Cleanup cleanup_upload,
                                         std::chrono::milliseconds timeout)
    : upload_(std::move(upload)), download_(std::move(download)),
      cancel_upload_(std::move(cancel_upload)), cancel_download_(std::move(cancel_download)),
      cleanup_upload_(std::move(cleanup_upload)),
      timeout_(timeout)
{
    if (!upload_ || !download_ || timeout_ <= std::chrono::milliseconds::zero()) {
        throw DomainError("Logos Storage callbacks and a positive timeout are required");
    }
}

std::string LogosStorageAdapter::put(const std::string& payload)
{
    {
        std::lock_guard lock(mutex_);
        if (starting_uploads_ >= maximum_pending_sessions ||
            waiting_uploads_.size() >= maximum_pending_sessions - starting_uploads_) {
            throw DomainError("too many pending Logos Storage uploads");
        }
        ++starting_uploads_;
    }
    std::string session;
    try {
        session = upload_(payload);
    } catch (...) {
        std::lock_guard lock(mutex_);
        --starting_uploads_;
        discardUnclaimed(uploads_, waiting_uploads_, starting_uploads_);
        throw;
    }
    if (session.empty()) {
        std::lock_guard lock(mutex_);
        --starting_uploads_;
        discardUnclaimed(uploads_, waiting_uploads_, starting_uploads_);
        throw DomainError("Logos Storage returned an empty upload session");
    }
    std::unique_lock lock(mutex_);
    --starting_uploads_;
    if (!waiting_uploads_.insert(session).second) {
        auto& pending = uploads_[session];
        pending.done = true;
        pending.success = false;
        pending.error = "duplicate upload session";
        rememberExpired(expired_uploads_, session);
        changed_.notify_all();
        discardUnclaimed(uploads_, waiting_uploads_, starting_uploads_);
        throw DomainError("Logos Storage returned a duplicate upload session");
    }
    discardUnclaimed(uploads_, waiting_uploads_, starting_uploads_);
    auto& pending = uploads_[session];
    if (!changed_.wait_for(lock, timeout_, [&pending] { return pending.done; })) {
        waiting_uploads_.erase(session);
        uploads_.erase(session);
        rememberExpired(expired_uploads_, session);
        lock.unlock();
        if (cancel_upload_) {
            cancel_upload_(session);
        }
        throw DomainError("Logos Storage upload timed out");
    }
    const auto result = pending;
    waiting_uploads_.erase(session);
    uploads_.erase(session);
    lock.unlock();
    if (cleanup_upload_) {
        cleanup_upload_(session);
    }
    if (!result.success || result.cid.empty()) {
        throw DomainError("Logos Storage upload failed: " + result.error);
    }
    return result.cid;
}

std::string LogosStorageAdapter::get(const std::string& address) const
{
    {
        std::lock_guard lock(mutex_);
        if (starting_downloads_ >= maximum_pending_sessions ||
            waiting_downloads_.size() >= maximum_pending_sessions - starting_downloads_) {
            throw DomainError("too many pending Logos Storage downloads");
        }
        ++starting_downloads_;
    }
    std::string session;
    try {
        session = download_(address);
    } catch (...) {
        std::lock_guard lock(mutex_);
        --starting_downloads_;
        discardUnclaimed(downloads_, waiting_downloads_, starting_downloads_);
        throw;
    }
    if (session.empty()) {
        std::lock_guard lock(mutex_);
        --starting_downloads_;
        discardUnclaimed(downloads_, waiting_downloads_, starting_downloads_);
        throw DomainError("Logos Storage returned an empty download session");
    }
    std::unique_lock lock(mutex_);
    --starting_downloads_;
    if (!waiting_downloads_.insert(session).second) {
        auto& pending = downloads_[session];
        pending.done = true;
        pending.success = false;
        pending.error = "duplicate download session";
        rememberExpired(expired_downloads_, session);
        changed_.notify_all();
        discardUnclaimed(downloads_, waiting_downloads_, starting_downloads_);
        throw DomainError("Logos Storage returned a duplicate download session");
    }
    discardUnclaimed(downloads_, waiting_downloads_, starting_downloads_);
    auto& pending = downloads_[session];
    if (!changed_.wait_for(lock, timeout_, [&pending] { return pending.done; })) {
        waiting_downloads_.erase(session);
        downloads_.erase(session);
        rememberExpired(expired_downloads_, session);
        lock.unlock();
        if (cancel_download_) {
            cancel_download_(session);
        }
        throw DomainError("Logos Storage download timed out");
    }
    const auto result = pending;
    waiting_downloads_.erase(session);
    downloads_.erase(session);
    if (!result.success) {
        throw DomainError("Logos Storage download failed: " + result.error);
    }
    return result.payload;
}

void LogosStorageAdapter::uploadDone(const std::string& event_json)
{
    const auto event = parseEvent(event_json);
    const auto session = requireString(event, "sessionId");
    const auto success = optionalBoolean(event, "success", false);
    const auto cid = optionalString(event, "cid", "");
    const auto error = optionalString(event, "error", "unknown error");
    std::lock_guard lock(mutex_);
    if (expired_uploads_.contains(session)) {
        return;
    }
    if (!uploads_.contains(session) &&
        (starting_uploads_ == 0 || uploads_.size() >= maximum_pending_sessions)) {
        return;
    }
    auto& pending = uploads_[session];
    pending.done = true;
    pending.success = success;
    pending.cid = cid;
    pending.error = error;
    changed_.notify_all();
}

void LogosStorageAdapter::downloadProgress(const std::string& event_json)
{
    const auto event = parseEvent(event_json);
    const auto session = requireString(event, "sessionId");
    if (!optionalBoolean(event, "success", false) || !event.contains("chunk")) {
        return;
    }
    const auto encoded = requireString(event, "chunk");
    if (encoded.size() > maximum_encoded_chunk_bytes) {
        bool cancel = false;
        {
            std::lock_guard lock(mutex_);
            if (expired_downloads_.contains(session)) {
                return;
            }
            if (!downloads_.contains(session) &&
                (starting_downloads_ == 0 || downloads_.size() >= maximum_pending_sessions)) {
                return;
            }
            auto& pending = downloads_[session];
            pending.done = true;
            pending.success = false;
            pending.error = "encoded chunk exceeds the download limit";
            rememberExpired(expired_downloads_, session);
            cancel = true;
            changed_.notify_all();
        }
        if (cancel && cancel_download_) {
            cancel_download_(session);
        }
        return;
    }
    const auto chunk = decodeBase64(encoded);
    bool cancel = false;
    {
        std::lock_guard lock(mutex_);
        if (expired_downloads_.contains(session)) {
            return;
        }
        if (!downloads_.contains(session) &&
            (starting_downloads_ == 0 || downloads_.size() >= maximum_pending_sessions)) {
            return;
        }
        auto& pending = downloads_[session];
        if (chunk.size() > maximum_download_bytes - pending.payload.size()) {
            pending.done = true;
            pending.success = false;
            pending.error = "encrypted object exceeds the download limit";
            rememberExpired(expired_downloads_, session);
            cancel = true;
            changed_.notify_all();
        } else {
            pending.payload += chunk;
        }
    }
    if (cancel && cancel_download_) {
        cancel_download_(session);
    }
}

void LogosStorageAdapter::downloadDone(const std::string& event_json)
{
    const auto event = parseEvent(event_json);
    const auto session = requireString(event, "sessionId");
    const auto success = optionalBoolean(event, "success", false);
    const auto error = optionalString(event, "error", "unknown error");
    std::lock_guard lock(mutex_);
    if (expired_downloads_.contains(session)) {
        return;
    }
    if (!downloads_.contains(session) &&
        (starting_downloads_ == 0 || downloads_.size() >= maximum_pending_sessions)) {
        return;
    }
    auto& pending = downloads_[session];
    pending.done = true;
    pending.success = success;
    pending.error = error;
    changed_.notify_all();
}

} // namespace bonded
