#pragma once

#include "domain/types.h"
#include "integrations/interfaces.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bonded {

struct AgentCard {
    std::string protocol;
    std::string network;
    std::string agent_id;
    std::string public_key;
    std::vector<std::string> skills;
    Json capabilities;
    std::string topic;
    std::uint64_t task_price{0};
    std::uint64_t expires_at{0};
    std::string signature;
};

enum class A2ATaskState { Working, InputRequired, Completed, Failed, Canceled };

struct A2ATask {
    std::string id;
    std::string requester;
    std::string provider;
    std::string skill;
    Json input;
    Json output;
    std::uint64_t price{0};
    std::uint64_t expires_at{0};
    A2ATaskState state{A2ATaskState::Working};
    std::string payment_reference;
    std::uint64_t revision{0};
};

class A2AService {
public:
    A2AService(ProgramAdapter& program, std::string network);

    static std::string canonicalCard(const AgentCard& card);
    static AgentCard signCard(AgentCard card, const std::string& private_key);
    bool verifyCard(const AgentCard& card, std::uint64_t now_unix) const;
    AgentCard publishCard(const AgentCard& card, std::uint64_t now_unix);
    std::vector<AgentCard> discover(const std::string& skill, std::uint64_t now_unix) const;

    A2ATask createTask(A2ATask task, std::uint64_t now_unix);
    A2ATask requireInput(const std::string& task_id);
    A2ATask complete(const std::string& task_id, const Json& output);
    A2ATask fail(const std::string& task_id, const std::string& reason);
    A2ATask subscribe(const std::string& task_id) const;
    A2ATask cancel(const std::string& task_id, std::uint64_t now_unix);
    std::size_t activeTaskCount() const;

private:
    ProgramAdapter& program_;
    std::string network_;
    mutable std::mutex mutex_;
    std::map<std::string, AgentCard> cards_;
    std::map<std::string, A2ATask> tasks_;
};

std::string toString(A2ATaskState state);
void to_json(Json& json, const AgentCard& card);
void from_json(const Json& json, AgentCard& card);
void to_json(Json& json, const A2ATask& task);
void from_json(const Json& json, A2ATask& task);

} // namespace bonded
