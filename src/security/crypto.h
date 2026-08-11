#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bonded {

struct Ciphertext {
    std::string nonce_hex;
    std::string data_hex;
    std::string tag_hex;
};

class Crypto {
public:
    static std::string sha256(const std::string& value);
    static std::pair<std::string, std::string> generateEd25519KeyPair();
    static std::string signEd25519(const std::string& private_key_hex,
                                   const std::string& message);
    static bool verifyEd25519(const std::string& public_key_hex, const std::string& message,
                              const std::string& signature_hex);
    static Ciphertext encryptAes256Gcm(const std::string& key_hex, const std::string& plaintext,
                                       const std::string& associated_data);
    static std::string decryptAes256Gcm(const std::string& key_hex,
                                       const Ciphertext& ciphertext,
                                       const std::string& associated_data);
    static std::string randomHex(std::size_t bytes);

    static std::string hexEncode(const unsigned char* data, std::size_t size);
    static std::vector<unsigned char> hexDecode(const std::string& value);
};

} // namespace bonded
