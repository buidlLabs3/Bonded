#include "integrations/logos_adapters.h"

#include "security/crypto.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <thread>

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

std::string amountLe16(std::uint64_t amount)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded(32, '0');
    for (std::size_t index = 0; index < sizeof(amount); ++index) {
        const auto byte = static_cast<unsigned>((amount >> (index * 8U)) & 0xffU);
        encoded[index * 2] = hex[byte >> 4U];
        encoded[index * 2 + 1] = hex[byte & 0x0fU];
    }
    return encoded;
}

bool isAccountId(const std::string& value)
{
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](const auto byte) {
               return std::isxdigit(static_cast<unsigned char>(byte)) != 0;
           });
}

std::string transactionHash(const std::string& response, const char* operation)
{
    try {
        const auto result = Json::parse(response);
        if (!result.is_object() || !result.value("success", false)) {
            const auto error = result.is_object() ? result.value("error", "unknown error")
                                                  : "invalid response";
            throw DomainError(std::string(operation) + " failed: " + error);
        }
        const auto hash = result.value("tx_hash", "");
        if (!isAccountId(hash)) {
            throw DomainError(std::string(operation) + " returned an invalid transaction hash");
        }
        return hash;
    } catch (const DomainError&) {
        throw;
    } catch (const std::exception&) {
        throw DomainError(std::string(operation) + " returned invalid JSON");
    }
}

void requireIncluded(const std::string& hash, const std::function<bool(const std::string&)>& poll,
                     std::size_t attempts, std::chrono::milliseconds interval,
                     const char* operation)
{
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        try {
            if (poll(hash)) {
                return;
            }
        } catch (const std::exception&) {
            if (attempt + 1 == attempts) {
                throw DomainError(std::string(operation) + " inclusion check failed");
            }
        }
        if (attempt + 1 < attempts && interval.count() > 0) {
            std::this_thread::sleep_for(interval);
        }
    }
    throw DomainError(std::string(operation) + " was not included before the timeout");
}

std::vector<std::uint8_t> readProgram(const std::string& binary_path)
{
    static constexpr std::uintmax_t maximum_program_bytes = 32U * 1024U * 1024U;
    const std::filesystem::path path(binary_path);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximum_program_bytes) {
        throw DomainError("LEZ program must be a non-empty regular file of at most 32 MiB");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw DomainError("could not open LEZ program binary");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        throw DomainError("could not read complete LEZ program binary");
    }
    return bytes;
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

LezWalletAdapter::LezWalletAdapter(std::string account_id, bool is_public, Balance balance,
                                   OwnedTransfer owned_transfer,
                                   PrivateTransfer private_transfer, Poll poll,
                                   LoadHistory load_history,
                                   RecordTransfer record_transfer, std::size_t poll_attempts,
                                   std::chrono::milliseconds poll_interval)
    : account_id_(std::move(account_id)), is_public_(is_public), balance_(std::move(balance)),
      owned_transfer_(std::move(owned_transfer)), private_transfer_(std::move(private_transfer)),
      poll_(std::move(poll)),
      load_history_(std::move(load_history)), record_transfer_(std::move(record_transfer)),
      poll_attempts_(poll_attempts), poll_interval_(poll_interval)
{
    if (!isAccountId(account_id_) || !balance_ || !owned_transfer_ || !private_transfer_ ||
        !poll_ || !load_history_ || !record_transfer_ || poll_attempts_ == 0) {
        throw DomainError("LEZ wallet account and callbacks are required");
    }
}

std::uint64_t LezWalletAdapter::balance() const
{
    const auto value = balance_(account_id_, is_public_);
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(), [](const auto byte) { return std::isdigit(byte); })) {
        throw DomainError("LEZ Core returned an invalid balance");
    }
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) {
            throw DomainError("LEZ Core returned an invalid balance");
        }
        return parsed;
    } catch (const DomainError&) {
        throw;
    } catch (const std::exception&) {
        throw DomainError("LEZ balance exceeds Bonded Inbox's uint64 limit");
    }
}

std::string LezWalletAdapter::send(const std::string& recipient, std::uint64_t amount,
                                   std::uint64_t now_unix)
{
    if (!isAccountId(recipient) || amount == 0) {
        throw DomainError("LEZ transfer recipient and positive amount are required");
    }
    const auto hash = transactionHash(
        owned_transfer_(account_id_, recipient, amountLe16(amount)), "LEZ transfer");
    requireIncluded(hash, poll_, poll_attempts_, poll_interval_, "LEZ transfer");
    record_transfer_(WalletTransfer{hash, recipient, amount, now_unix});
    return hash;
}

std::string LezWalletAdapter::sendPrivate(const std::string& recipient,
                                          const Json& recipient_keys,
                                          std::uint64_t amount,
                                          std::uint64_t now_unix)
{
    if (!recipient_keys.is_object()) {
        throw DomainError("LEZ private transfer recipient keys are invalid");
    }
    const auto nullifier_key = recipient_keys.value("nullifier_public_key", "");
    const auto viewing_key = recipient_keys.value("viewing_public_key", "");
    if (!isAccountId(recipient) || amount == 0 || !isAccountId(nullifier_key) ||
        viewing_key.size() % 2 != 0 ||
        (!viewing_key.empty() && Crypto::hexDecode(viewing_key).empty())) {
        throw DomainError("LEZ private transfer recipient keys are invalid");
    }
    const auto hash = transactionHash(
        private_transfer_(account_id_, recipient_keys, amountLe16(amount)),
        "LEZ private transfer");
    requireIncluded(hash, poll_, poll_attempts_, poll_interval_, "LEZ private transfer");
    record_transfer_(WalletTransfer{hash, recipient, amount, now_unix});
    return hash;
}

std::vector<WalletTransfer> LezWalletAdapter::history() const
{
    return load_history_();
}

LezProgramAdapter::LezProgramAdapter(Query query_public, Query query_private, Call call_public,
                                     Call call_private, Deploy deploy, Poll poll,
                                     std::size_t poll_attempts,
                                     std::chrono::milliseconds poll_interval)
    : query_public_(std::move(query_public)), query_private_(std::move(query_private)),
      call_public_(std::move(call_public)), call_private_(std::move(call_private)),
      deploy_(std::move(deploy)), poll_(std::move(poll)), poll_attempts_(poll_attempts),
      poll_interval_(poll_interval)
{
    if (!query_public_ || !query_private_ || !call_public_ || !call_private_ || !deploy_ ||
        !poll_ || poll_attempts_ == 0) {
        throw DomainError("LEZ program callbacks are required");
    }
}

Json LezProgramAdapter::query(const std::string& program_id, const Json& parameters) const
{
    if (program_id.empty() || !parameters.is_object()) {
        throw DomainError("LEZ program query is invalid");
    }
    const auto privacy = parameters.value("privacy", "private");
    const auto response = privacy == "public"    ? query_public_(program_id)
                          : privacy == "private" ? query_private_(program_id)
                                                 : throw DomainError(
                                                       "LEZ query privacy must be public or private");
    try {
        const auto result = Json::parse(response);
        if (!result.is_object()) {
            throw DomainError("LEZ account query returned a non-object result");
        }
        return result;
    } catch (const DomainError&) {
        throw;
    } catch (const std::exception&) {
        throw DomainError("LEZ account query returned invalid JSON");
    }
}

std::string LezProgramAdapter::call(const std::string& program_id,
                                    const std::string& instruction, const Json& parameters)
{
    if (program_id.empty() || !parameters.is_object()) {
        throw DomainError("LEZ program call is invalid");
    }
    if (instruction == "public") {
        const auto hash =
            transactionHash(call_public_(program_id, parameters), "LEZ public program call");
        requireIncluded(hash, poll_, poll_attempts_, poll_interval_, "LEZ public program call");
        return hash;
    }
    if (instruction == "private") {
        const auto hash =
            transactionHash(call_private_(program_id, parameters), "LEZ private program call");
        requireIncluded(hash, poll_, poll_attempts_, poll_interval_, "LEZ private program call");
        return hash;
    }
    throw DomainError("LEZ program instruction must be public or private");
}

std::string LezProgramAdapter::deploy(const std::string& binary_path)
{
    const auto hash =
        transactionHash(deploy_(readProgram(binary_path)), "LEZ program deployment");
    requireIncluded(hash, poll_, poll_attempts_, poll_interval_, "LEZ program deployment");
    return hash;
}

} // namespace bonded
