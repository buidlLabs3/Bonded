#include "services/messaging_service.h"

#include "security/crypto.h"

namespace bonded {
namespace {

std::string encryptionContext(const SignedEnvelope& envelope)
{
    return Json{{"protocol", envelope.protocol},
                {"network", envelope.network},
                {"id", envelope.id},
                {"sender", envelope.sender},
                {"recipient", envelope.recipient},
                {"topic", envelope.topic},
                {"nonce", envelope.nonce},
                {"expires_at", envelope.expires_at},
                {"ephemeral_public_key", envelope.ephemeral_public_key},
                {"public_key", envelope.public_key}}
        .dump();
}

bool validEncryptionShape(const SignedEnvelope& envelope)
{
    try {
        return !envelope.ciphertext.empty() &&
               Crypto::hexDecode(envelope.ciphertext).size() > 0 &&
               Crypto::hexDecode(envelope.encryption_nonce).size() == 12 &&
               Crypto::hexDecode(envelope.authentication_tag).size() == 16 &&
               Crypto::hexDecode(envelope.ephemeral_public_key).size() == 32;
    } catch (const DomainError&) {
        return false;
    }
}

} // namespace

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
    json.erase("signature");
    return json.dump();
}

SignedEnvelope MessagingService::sealAndSign(
    SignedEnvelope envelope, const std::string& plaintext,
    const std::string& recipient_encryption_public_key,
    const std::string& signing_private_key, const std::string& signing_public_key)
{
    if (envelope.protocol != "bonded-inbox/envelope/v2" || envelope.network.empty() ||
        envelope.id.empty() || envelope.sender.empty() || envelope.recipient.empty() ||
        envelope.topic.empty() || envelope.nonce.empty() || envelope.expires_at == 0 ||
        plaintext.empty()) {
        throw DomainError("incomplete messaging envelope");
    }
    if (!envelope.ciphertext.empty() || !envelope.encryption_nonce.empty() ||
        !envelope.authentication_tag.empty() || !envelope.ephemeral_public_key.empty() ||
        !envelope.public_key.empty() || !envelope.signature.empty()) {
        throw DomainError("messaging envelope is already sealed");
    }
    const auto ephemeral = Crypto::generateX25519KeyPair();
    envelope.ephemeral_public_key = ephemeral.second;
    envelope.public_key = signing_public_key;
    const auto shared = Crypto::deriveX25519(ephemeral.first, recipient_encryption_public_key);
    const auto context = encryptionContext(envelope);
    const auto encryption_key = Crypto::hkdfSha256(shared, context);
    const auto encrypted = Crypto::encryptAes256Gcm(encryption_key, plaintext, context);
    envelope.ciphertext = encrypted.data_hex;
    envelope.encryption_nonce = encrypted.nonce_hex;
    envelope.authentication_tag = encrypted.tag_hex;
    envelope.signature = Crypto::signEd25519(signing_private_key, canonicalUnsigned(envelope));
    return envelope;
}

bool MessagingService::verify(const SignedEnvelope& envelope, std::uint64_t now_unix,
                              const std::string& expected_network)
{
    try {
        return envelope.protocol == "bonded-inbox/envelope/v2" &&
               envelope.network == expected_network && envelope.expires_at >= now_unix &&
               !envelope.id.empty() && !envelope.sender.empty() && !envelope.recipient.empty() &&
               !envelope.topic.empty() && !envelope.nonce.empty() &&
               !envelope.public_key.empty() && validEncryptionShape(envelope) &&
               Crypto::verifyEd25519(envelope.public_key, canonicalUnsigned(envelope),
                                     envelope.signature);
    } catch (const DomainError&) {
        return false;
    }
}

std::string MessagingService::open(const SignedEnvelope& envelope, std::uint64_t now_unix,
                                   const std::string& expected_network,
                                   const std::string& expected_recipient,
                                   const std::string& recipient_encryption_private_key,
                                   const std::string& expected_sender,
                                   const std::string& expected_sender_public_key)
{
    if (!verify(envelope, now_unix, expected_network) ||
        envelope.recipient != expected_recipient ||
        envelope.sender != expected_sender ||
        envelope.public_key != expected_sender_public_key) {
        throw DomainError("received envelope identity is unauthorized");
    }
    const auto context = encryptionContext(envelope);
    const auto shared =
        Crypto::deriveX25519(recipient_encryption_private_key, envelope.ephemeral_public_key);
    const auto encryption_key = Crypto::hkdfSha256(shared, context);
    return Crypto::decryptAes256Gcm(
        encryption_key,
        Ciphertext{envelope.encryption_nonce, envelope.ciphertext,
                   envelope.authentication_tag},
        context);
}

std::string MessagingService::send(const SignedEnvelope& envelope, std::uint64_t now_unix)
{
    if (!verify(envelope, now_unix, network_)) {
        throw DomainError("cannot send invalid messaging envelope");
    }
    return adapter_.send(envelope.topic, Json(envelope).dump());
}

void MessagingService::subscribe(const std::string& topic,
                                 std::function<std::uint64_t()> clock,
                                 const std::string& expected_recipient,
                                 const std::string& recipient_encryption_private_key,
                                 const std::string& expected_sender,
                                 const std::string& expected_sender_public_key,
                                 std::function<void(const SignedEnvelope&, const std::string&)>
                                     handler)
{
    if (!clock || expected_recipient.empty() || recipient_encryption_private_key.empty() ||
        expected_sender.empty() || expected_sender_public_key.empty() || !handler) {
        throw DomainError("owner channel identity, keys, and handler are required");
    }
    adapter_.subscribe(topic, [this, topic, clock = std::move(clock), expected_recipient,
                               recipient_encryption_private_key, expected_sender,
                               expected_sender_public_key,
                               handler = std::move(handler)](const std::string& raw) {
        SignedEnvelope envelope;
        std::string plaintext;
        try {
            envelope = Json::parse(raw).get<SignedEnvelope>();
            if (envelope.topic != topic) {
                throw DomainError("received envelope topic is unauthorized");
            }
            plaintext = open(envelope, clock(), network_, expected_recipient,
                             recipient_encryption_private_key, expected_sender,
                             expected_sender_public_key);
        } catch (const std::exception&) {
            throw DomainError("received invalid messaging envelope");
        }
        {
            std::lock_guard lock(mutex_);
            if (!processed_ids_.insert(envelope.id + ":" + envelope.nonce).second) {
                return;
            }
        }
        handler(envelope, plaintext);
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
                {"nonce", envelope.nonce},
                {"expires_at", envelope.expires_at},
                {"ciphertext", envelope.ciphertext},
                {"encryption_nonce", envelope.encryption_nonce},
                {"authentication_tag", envelope.authentication_tag},
                {"ephemeral_public_key", envelope.ephemeral_public_key},
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
    json.at("nonce").get_to(envelope.nonce);
    json.at("expires_at").get_to(envelope.expires_at);
    json.at("ciphertext").get_to(envelope.ciphertext);
    json.at("encryption_nonce").get_to(envelope.encryption_nonce);
    json.at("authentication_tag").get_to(envelope.authentication_tag);
    json.at("ephemeral_public_key").get_to(envelope.ephemeral_public_key);
    json.at("public_key").get_to(envelope.public_key);
    json.at("signature").get_to(envelope.signature);
}

} // namespace bonded
