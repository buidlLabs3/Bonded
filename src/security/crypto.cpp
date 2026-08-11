#include "security/crypto.h"

#include "domain/types.h"

#include <array>
#include <memory>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace bonded {
namespace {

template <typename T, void (*Free)(T*)> using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw DomainError(message);
    }
}

} // namespace

std::string Crypto::hexEncode(const unsigned char* data, std::size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = kHex[data[index] >> 4U];
        result[index * 2 + 1] = kHex[data[index] & 0x0fU];
    }
    return result;
}

std::vector<unsigned char> Crypto::hexDecode(const std::string& value)
{
    if (value.size() % 2 != 0) {
        throw DomainError("hex value has odd length");
    }
    auto nibble = [](char character) -> unsigned char {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned char>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<unsigned char>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F') {
            return static_cast<unsigned char>(character - 'A' + 10);
        }
        throw DomainError("invalid hex value");
    };
    std::vector<unsigned char> result(value.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<unsigned char>((nibble(value[index * 2]) << 4U) |
                                                   nibble(value[index * 2 + 1]));
    }
    return result;
}

std::string Crypto::sha256(const std::string& value)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    OpenSslPtr<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(context != nullptr, "cannot allocate digest context");
    require(EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1,
            "cannot initialize SHA-256");
    require(EVP_DigestUpdate(context.get(), value.data(), value.size()) == 1,
            "cannot update SHA-256");
    require(EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) == 1,
            "cannot finalize SHA-256");
    return hexEncode(digest.data(), digest_size);
}

std::pair<std::string, std::string> Crypto::generateEd25519KeyPair()
{
    OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                                                        EVP_PKEY_CTX_free);
    require(context != nullptr, "cannot allocate Ed25519 context");
    require(EVP_PKEY_keygen_init(context.get()) == 1, "cannot initialize Ed25519 keygen");
    EVP_PKEY* raw_key = nullptr;
    require(EVP_PKEY_keygen(context.get(), &raw_key) == 1, "cannot generate Ed25519 key");
    OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(raw_key, EVP_PKEY_free);

    std::array<unsigned char, 32> private_key{};
    std::array<unsigned char, 32> public_key{};
    std::size_t private_size = private_key.size();
    std::size_t public_size = public_key.size();
    require(EVP_PKEY_get_raw_private_key(key.get(), private_key.data(), &private_size) == 1,
            "cannot export Ed25519 private key");
    require(EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &public_size) == 1,
            "cannot export Ed25519 public key");
    return {hexEncode(private_key.data(), private_size), hexEncode(public_key.data(), public_size)};
}

std::string Crypto::signEd25519(const std::string& private_key_hex, const std::string& message)
{
    const auto private_key = hexDecode(private_key_hex);
    require(private_key.size() == 32, "Ed25519 private key must be 32 bytes");
    OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key.data(), private_key.size()),
        EVP_PKEY_free);
    require(key != nullptr, "cannot import Ed25519 private key");
    OpenSslPtr<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(context != nullptr, "cannot allocate signature context");
    require(EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1,
            "cannot initialize Ed25519 signing");
    std::array<unsigned char, 64> signature{};
    std::size_t signature_size = signature.size();
    require(EVP_DigestSign(context.get(), signature.data(), &signature_size,
                           reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1,
            "cannot sign Ed25519 message");
    return hexEncode(signature.data(), signature_size);
}

bool Crypto::verifyEd25519(const std::string& public_key_hex, const std::string& message,
                           const std::string& signature_hex)
{
    const auto public_key = hexDecode(public_key_hex);
    const auto signature = hexDecode(signature_hex);
    if (public_key.size() != 32 || signature.size() != 64) {
        return false;
    }
    OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()),
        EVP_PKEY_free);
    if (key == nullptr) {
        return false;
    }
    OpenSslPtr<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (context == nullptr ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return false;
    }
    return EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()),
                            message.size()) == 1;
}

Ciphertext Crypto::encryptAes256Gcm(const std::string& key_hex, const std::string& plaintext,
                                    const std::string& associated_data)
{
    const auto key = hexDecode(key_hex);
    require(key.size() == 32, "AES-256-GCM key must be 32 bytes");
    std::array<unsigned char, 12> nonce{};
    require(RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) == 1,
            "cannot generate AES-GCM nonce");
    OpenSslPtr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> context(EVP_CIPHER_CTX_new(),
                                                            EVP_CIPHER_CTX_free);
    require(context != nullptr, "cannot allocate cipher context");
    require(EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1,
            "cannot initialize AES-GCM");
    require(EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) == 1,
            "cannot set AES-GCM key");
    int written = 0;
    if (!associated_data.empty()) {
        require(EVP_EncryptUpdate(context.get(), nullptr, &written,
                                  reinterpret_cast<const unsigned char*>(associated_data.data()),
                                  static_cast<int>(associated_data.size())) == 1,
                "cannot authenticate AES-GCM associated data");
    }
    std::vector<unsigned char> encrypted(plaintext.size() + 16);
    require(EVP_EncryptUpdate(context.get(), encrypted.data(), &written,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) == 1,
            "cannot encrypt AES-GCM plaintext");
    int total = written;
    require(EVP_EncryptFinal_ex(context.get(), encrypted.data() + total, &written) == 1,
            "cannot finalize AES-GCM encryption");
    total += written;
    encrypted.resize(static_cast<std::size_t>(total));
    std::array<unsigned char, 16> tag{};
    require(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1,
            "cannot read AES-GCM tag");
    return {hexEncode(nonce.data(), nonce.size()), hexEncode(encrypted.data(), encrypted.size()),
            hexEncode(tag.data(), tag.size())};
}

std::string Crypto::decryptAes256Gcm(const std::string& key_hex, const Ciphertext& ciphertext,
                                    const std::string& associated_data)
{
    const auto key = hexDecode(key_hex);
    const auto nonce = hexDecode(ciphertext.nonce_hex);
    const auto encrypted = hexDecode(ciphertext.data_hex);
    const auto tag = hexDecode(ciphertext.tag_hex);
    require(key.size() == 32, "AES-256-GCM key must be 32 bytes");
    require(nonce.size() == 12 && tag.size() == 16, "invalid AES-GCM envelope");
    OpenSslPtr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> context(EVP_CIPHER_CTX_new(),
                                                            EVP_CIPHER_CTX_free);
    require(context != nullptr, "cannot allocate cipher context");
    require(EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1,
            "cannot initialize AES-GCM");
    require(EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) == 1,
            "cannot set AES-GCM key");
    int written = 0;
    if (!associated_data.empty()) {
        require(EVP_DecryptUpdate(context.get(), nullptr, &written,
                                  reinterpret_cast<const unsigned char*>(associated_data.data()),
                                  static_cast<int>(associated_data.size())) == 1,
                "cannot authenticate AES-GCM associated data");
    }
    std::vector<unsigned char> plaintext(encrypted.size() + 1);
    require(EVP_DecryptUpdate(context.get(), plaintext.data(), &written, encrypted.data(),
                              static_cast<int>(encrypted.size())) == 1,
            "cannot decrypt AES-GCM ciphertext");
    int total = written;
    require(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, tag.size(),
                                const_cast<unsigned char*>(tag.data())) == 1,
            "cannot set AES-GCM tag");
    require(EVP_DecryptFinal_ex(context.get(), plaintext.data() + total, &written) == 1,
            "AES-GCM authentication failed");
    total += written;
    return {reinterpret_cast<const char*>(plaintext.data()), static_cast<std::size_t>(total)};
}

std::string Crypto::randomHex(std::size_t bytes)
{
    std::vector<unsigned char> random(bytes);
    require(RAND_bytes(random.data(), static_cast<int>(random.size())) == 1,
            "cannot generate random bytes");
    return hexEncode(random.data(), random.size());
}

} // namespace bonded
