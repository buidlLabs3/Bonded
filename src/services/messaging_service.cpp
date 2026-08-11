#include "services/messaging_service.h"

#include "security/crypto.h"

namespace bonded {

MessagingService::MessagingService(MessagingAdapter& adapter, std::string network)
    : adapter_(adapter), network_(std::move(network))
{
    if (network_.empty()) {
        throw DomainError("messaging network is required");
    }
}

std::string MessagingService::canonicalUnsigned(const SignedEnvelope& envelope)
{
    Json json = envelope;
    json.erase("public_key");
    json.erase("signature");
    return json.dump();
}

SignedEnvelope MessagingService::sign(SignedEnvelope envelope, const std::string& private_key,
                                      const std::string& public_key)
{
    if (envelope.protocol != "bonded-inbox/envelope/v1" || envelope.network.empty() ||
        envelope.id.empty() || envelope.sender.empty() || envelope.recipient.empty() ||
        envelope.topic.empty() || envelope.nonce.empty() || envelope.expires_at == 0) {
        throw DomainError("incomplete messaging envelope");
    }
    envelope.public_key = public_key;
    envelope.signature = Crypto::signEd25519(private_key, canonicalUnsigned(envelope));
    return envelope;
}

bool MessagingService::verify(const SignedEnvelope& envelope, std::uint64_t now_unix,
                              const std::string& expected_network)
{
    return envelope.protocol == "bonded-inbox/envelope/v1" &&
           envelope.network == expected_network && envelope.expires_at >= now_unix &&
           !envelope.id.empty() && !envelope.sender.empty() && !envelope.recipient.empty() &&
           !envelope.topic.empty() && !envelope.nonce.empty() && !envelope.public_key.empty() &&
           Crypto::verifyEd25519(envelope.public_key, canonicalUnsigned(envelope),
                                 envelope.signature);
}

std::string MessagingService::send(const SignedEnvelope& envelope, std::uint64_t now_unix)
{
    if (!verify(envelope, now_unix, network_)) {
        throw DomainError("cannot send invalid messaging envelope");
    }
    return adapter_.send(envelope.topic, Json(envelope).dump());
}

void MessagingService::subscribe(const std::string& topic, std::uint64_t now_unix,
                                 std::function<void(const SignedEnvelope&)> handler)
{
    adapter_.subscribe(topic, [this, now_unix, handler = std::move(handler)](const std::string& raw) {
        const auto envelope = Json::parse(raw).get<SignedEnvelope>();
        if (!verify(envelope, now_unix, network_)) {
            throw DomainError("received invalid messaging envelope");
        }
        {
            std::lock_guard lock(mutex_);
            if (!processed_ids_.insert(envelope.id + ":" + envelope.nonce).second) {
                return;
            }
        }
        handler(envelope);
    });
}

void to_json(Json& json, const SignedEnvelope& envelope)
{
    json = Json{{"protocol", envelope.protocol},
                {"network", envelope.network},
                {"id", envelope.id},
                {"sender", envelope.sender},
                {"recipient", envelope.recipient},
                {"topic", envelope.topic},
                {"payload", envelope.payload},
                {"nonce", envelope.nonce},
                {"expires_at", envelope.expires_at},
                {"public_key", envelope.public_key},
                {"signature", envelope.signature}};
}

void from_json(const Json& json, SignedEnvelope& envelope)
{
    json.at("protocol").get_to(envelope.protocol);
    json.at("network").get_to(envelope.network);
    json.at("id").get_to(envelope.id);
    json.at("sender").get_to(envelope.sender);
    json.at("recipient").get_to(envelope.recipient);
    json.at("topic").get_to(envelope.topic);
    json.at("payload").get_to(envelope.payload);
    json.at("nonce").get_to(envelope.nonce);
    json.at("expires_at").get_to(envelope.expires_at);
    json.at("public_key").get_to(envelope.public_key);
    json.at("signature").get_to(envelope.signature);
}

} // namespace bonded
