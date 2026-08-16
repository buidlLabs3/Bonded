#include "runtime/default_skill_catalog.h"

namespace bonded {

const std::vector<DefaultSkillSpec>& allDefaultSkillSpecs()
{
    using enum Profile;
    static const std::vector<DefaultSkillSpec> specs{
        {"storage.upload", "Encrypt and upload an object", {Vault}},
        {"storage.download", "Download and decrypt an object", {Vault}},
        {"storage.list", "List local encrypted object metadata", {Vault}},
        {"storage.share", "Create an expiring encrypted storage grant", {Vault}},
        {"messaging.send", "Send a signed Logos Messaging envelope", {Inbox}},
        {"messaging.join", "Join a Logos Messaging group", {Inbox}},
        {"messaging.create_group", "Create a Logos Messaging group", {Inbox}},
        {"wallet.balance", "Return the shielded wallet balance", {Settlement}},
        {"wallet.send", "Send within limits or request owner approval", {Settlement}},
        {"wallet.history", "Return redacted transfer history", {Settlement}},
        {"program.query", "Query a LEZ program", {Settlement}},
        {"program.call", "Call a LEZ program instruction", {Settlement}},
        {"program.deploy", "Deploy a LEZ program binary", {Settlement}},
        {"agent.card", "Get or publish a signed A2A Agent Card", {Inbox, Vault, Settlement}},
        {"agent.discover", "Discover valid A2A Agent Cards", {Inbox, Vault, Settlement}},
        {"agent.task", "Create or complete a paid A2A task", {Inbox, Vault, Settlement}},
        {"agent.subscribe", "Read current A2A task state", {Inbox, Vault, Settlement}},
        {"agent.cancel", "Cancel an A2A task before completion", {Inbox, Vault, Settlement}},
        {"meta.skills", "List profile-allowed skill schemas", {Inbox, Vault, Settlement}},
        {"meta.status", "Return private-data-safe runtime health", {Inbox, Vault, Settlement}},
        {"meta.configure", "Apply an owner-signed atomic configuration update",
         {Inbox, Vault, Settlement}},
    };
    return specs;
}

std::set<std::string> requiredDefaultSkillNames()
{
    std::set<std::string> result;
    for (const auto& spec : allDefaultSkillSpecs()) {
        result.insert(spec.name);
    }
    return result;
}

} // namespace bonded
