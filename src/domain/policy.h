#pragma once

#include "domain/types.h"

#include <string>

namespace bonded {

class PolicyService {
public:
    static void validate(const InboxPolicy& policy);
    static std::string canonicalUnsigned(const InboxPolicy& policy);
    static std::string hash(const InboxPolicy& policy);
    static InboxPolicy sign(InboxPolicy policy, const std::string& private_key_hex,
                            const std::string& public_key_hex);
    static bool verify(const InboxPolicy& policy);
    static void requireSuccessor(const InboxPolicy& current, const InboxPolicy& successor);
};

} // namespace bonded
