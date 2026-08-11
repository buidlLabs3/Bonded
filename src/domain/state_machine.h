#pragma once

#include "domain/types.h"

#include <vector>

namespace bonded {

class MessageStateMachine {
public:
    static bool canTransition(MessageState from, MessageState to);
    static void transition(MessageRecord& message, MessageState to, std::uint64_t expected_revision);
    static std::vector<MessageState> allowedTransitions(MessageState from);
    static bool isDecision(MessageState state);
    static SettlementOutcome requiredSettlement(MessageState decision);
};

} // namespace bonded
