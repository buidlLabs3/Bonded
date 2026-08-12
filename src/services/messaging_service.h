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
    std::string nonce;
    std::uint64_t expires_at{0};
    std::string ciphertext;
    std::string encryption_nonce;
    std::string authentication_tag;
    std::string ephemeral_public_key;
    std::string public_key;
    std::string signature;
};

class MessagingService {
public:
    MessagingService(MessagingAdapter& adapter, std::string network);

    static std::string canonicalUnsigned(const SignedEnvelope& envelope);
    static SignedEnvelope sealAndSign(SignedEnvelope envelope, const std::string& plaintext,
                                      const std::string& recipient_encryption_public_key,
                                      const std::string& signing_private_key,
                                      const std::string& signing_public_key);
    static bool verify(const SignedEnvelope& envelope, std::uint64_t now_unix,
                       const std::string& expected_network);
    static std::string open(const SignedEnvelope& envelope, std::uint64_t now_unix,
                            const std::string& expected_network,
                            const std::string& expected_recipient,
                            const std::string& recipient_encryption_private_key,
                            const std::string& expected_sender,
                            const std::string& expected_sender_public_key);
    std::string send(const SignedEnvelope& envelope, std::uint64_t now_unix);
    void subscribe(const std::string& topic, std::function<std::uint64_t()> clock,
                   const std::string& expected_recipient,
                   const std::string& recipient_encryption_private_key,
                   const std::string& expected_sender,
                   const std::string& expected_sender_public_key,
                   std::function<void(const SignedEnvelope&, const std::string&)> handler);

private:
    MessagingAdapter& adapter_;
    std::string network_;
    std::mutex mutex_;
    std::set<std::string> processed_ids_;
};

void to_json(Json& json, const SignedEnvelope& envelope);
void from_json(const Json& json, SignedEnvelope& envelope);

} // namespace bonded
