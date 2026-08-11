#pragma once

#include "integrations/interfaces.h"

#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace bonded {

struct SignedEnvelope {
    std::string protocol;
    std::string network;
    std::string id;
    std::string sender;
    std::string recipient;
    std::string topic;
    std::string payload;
    std::string nonce;
    std::uint64_t expires_at{0};
    std::string public_key;
    std::string signature;
};

class MessagingService {
public:
    MessagingService(MessagingAdapter& adapter, std::string network);

    static std::string canonicalUnsigned(const SignedEnvelope& envelope);
    static SignedEnvelope sign(SignedEnvelope envelope, const std::string& private_key,
                               const std::string& public_key);
    static bool verify(const SignedEnvelope& envelope, std::uint64_t now_unix,
                       const std::string& expected_network);
    std::string send(const SignedEnvelope& envelope, std::uint64_t now_unix);
    void subscribe(const std::string& topic, std::uint64_t now_unix,
                   std::function<void(const SignedEnvelope&)> handler);

private:
    MessagingAdapter& adapter_;
    std::string network_;
    std::mutex mutex_;
    std::set<std::string> processed_ids_;
};

void to_json(Json& json, const SignedEnvelope& envelope);
void from_json(const Json& json, SignedEnvelope& envelope);

} // namespace bonded
