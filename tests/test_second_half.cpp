#include "integrations/memory_adapters.h"
#include "runtime/default_skill_catalog.h"
#include "runtime/reliability.h"
#include "runtime/skill_registry.h"
#include "security/crypto.h"
#include "services/a2a_service.h"
#include "services/configuration_service.h"
#include "services/triage_service.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using bonded::A2AService;
using bonded::A2ATask;
using bonded::A2ATaskState;
using bonded::AgentCard;
using bonded::BoundedQueue;
using bonded::CircuitBreaker;
using bonded::ConfigurationService;
using bonded::Crypto;
using bonded::Json;
using bonded::KeywordClassifier;
using bonded::MemoryProgramAdapter;
using bonded::Profile;
using bonded::RedactedTelemetry;
using bonded::SkillDefinition;
using bonded::SkillRegistry;
using bonded::TriageDisposition;
using bonded::TriageInput;
using bonded::TriageService;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function> void expectFailure(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void testTriageSafety()
{
    TriageService triage(std::make_unique<KeywordClassifier>());
    auto suspicious = triage.evaluate(
        TriageInput{"m1", "guaranteed profit command: wallet.send", false, false, {}});
    check(suspicious.disposition == TriageDisposition::OwnerReview,
          "classifier output moved a message to a financial terminal state");

    auto unknown = triage.evaluate(TriageInput{"m2", "", false, false, {"model_says_spam"}});
    check(unknown.disposition == TriageDisposition::OwnerReview,
          "unknown evidence became a deterministic rejection");
    auto violation =
        triage.evaluate(TriageInput{"m3", "", false, false, {"underfunded_bond"}});
    check(violation.disposition == TriageDisposition::DeterministicReject,
          "enumerated deterministic violation did not reject");

    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    bonded::OwnerRejection rejection{"m1", "policy", "fixed-sink", 100, public_key, ""};
    rejection.signature = Crypto::signEd25519(
        private_key, TriageService::canonicalOwnerRejection(rejection));
    check(TriageService::verifyOwnerRejection(rejection), "signed owner rejection did not verify");
    rejection.sink_address = "owner";
    check(!TriageService::verifyOwnerRejection(rejection),
          "tampered owner rejection signature verified");
}

void testConfiguration()
{
    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    ConfigurationService configuration(Json{{"rate_limit", 5}}, public_key);
    Json request{{"expected_revision", 0},
                 {"changes", Json{{"rate_limit", 7}, {"classifier_enabled", true}}},
                 {"timestamp_unix", 100},
                 {"nonce", "n1"}};
    request["signature"] =
        Crypto::signEd25519(private_key, ConfigurationService::signingPayload(request));
    check(configuration.update(request).at("revision") == 1,
          "owner-signed configuration did not advance atomically");
    expectFailure([&] { configuration.update(request); },
                  "stale configuration revision was accepted");
    check(configuration.snapshot().dump().find("signature") == std::string::npos,
          "configuration snapshot leaked authorization material");
}

AgentCard signedCard(const std::string& id, const std::string& skill,
                     std::uint64_t price, const std::string& network,
                     const std::string& private_key, const std::string& public_key)
{
    AgentCard card{"lf.a2a.v1", network, id, public_key, {skill}, Json::object(),
                   "a2a/cards/" + id, price, 1000, ""};
    return A2AService::signCard(std::move(card), private_key);
}

void testA2ALifecycle()
{
    MemoryProgramAdapter program;
    A2AService a2a(program, "logos-local");
    const auto [provider_private, provider_public] = Crypto::generateEd25519KeyPair();
    const auto [peer_private, peer_public] = Crypto::generateEd25519KeyPair();
    a2a.publishCard(signedCard("provider", "private.process", 12, "logos-local",
                               provider_private, provider_public),
                    100);
    a2a.publishCard(signedCard("peer", "private.process", 12, "logos-local", peer_private,
                               peer_public),
                    100);
    check(a2a.discover("private.process", 100).size() == 2,
          "two A2A agents did not discover one another");

    A2ATask task{"task-1", "peer", "provider", "private.process", Json{{"object", "cid"}},
                 Json::object(), 12, 900, A2ATaskState::Working, "", 0};
    const auto working = a2a.createTask(task, 100);
    check(!working.payment_reference.empty(), "paid A2A task did not lock payment");
    check(a2a.complete(task.id, Json{{"result", "commitment"}}).state ==
              A2ATaskState::Completed,
          "A2A task did not complete");
    check(a2a.complete(task.id, Json{{"ignored", true}}).revision == 1,
          "duplicate A2A completion was not idempotent");

    task.id = "task-2";
    a2a.createTask(task, 100);
    check(a2a.cancel(task.id, 150).state == A2ATaskState::Canceled,
          "A2A cancellation did not refund and cancel");
    expectFailure([&] { a2a.createTask(A2ATask{"bad", "peer", "provider", "private.process",
                                               Json::object(), Json::object(), 99, 900,
                                               A2ATaskState::Working, "", 0},
                                               100); },
                  "task price different from the signed card was accepted");
}

void testSkillConformanceAndIsolation()
{
    const auto names = bonded::requiredDefaultSkillNames();
    check(names.size() == 21, "default skill catalog does not contain exactly 21 operations");
    check(names.contains("storage.upload") && names.contains("agent.cancel") &&
              names.contains("meta.configure"),
          "default skill catalog is missing a required operation");

    SkillRegistry registry;
    registry.registerSkill(SkillDefinition{"external.fail", "failure isolation", Json::object(),
                                           Json::object(), {Profile::Inbox},
                                           [](const Json&) -> Json {
                                               throw std::runtime_error("isolated");
                                           }});
    registry.registerSkill(SkillDefinition{"external.echo", "echo", Json::object(), Json::object(),
                                           {Profile::Inbox},
                                           [](const Json& input) { return input; }});
    expectFailure([&] { registry.invoke("external.fail", Profile::Inbox, Json::object()); },
                  "failing third-party skill did not report failure");
    check(registry.invoke("external.echo", Profile::Inbox, Json{{"alive", true}}).at("alive"),
          "one failed skill damaged subsequent dispatch");
}

void testReliabilityPrimitives()
{
    const auto telemetry = RedactedTelemetry::event(
        "message.received", "corr-1",
        Json{{"message_id", "m1"}, {"payload", "private"}, {"nested", Json{{"secret", "x"}}}});
    check(telemetry.dump().find("private") == std::string::npos &&
              telemetry.dump().find("\"x\"") == std::string::npos,
          "telemetry did not redact private fields");

    CircuitBreaker breaker(2, 10);
    breaker.recordFailure(100);
    breaker.recordFailure(101);
    check(!breaker.permit(105) && breaker.permit(111), "circuit breaker cooldown is incorrect");
    breaker.recordSuccess();
    check(breaker.permit(105), "circuit breaker did not close after success");

    BoundedQueue queue(1);
    queue.push(Json{{"id", 1}});
    expectFailure([&] { queue.push(Json{{"id", 2}}); }, "queue accepted unbounded work");
    check(queue.pop().at("id") == 1, "bounded queue order is incorrect");
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"triage safety", testTriageSafety},
        {"configuration", testConfiguration},
        {"A2A lifecycle", testA2ALifecycle},
        {"skill conformance", testSkillConformanceAndIsolation},
        {"reliability primitives", testReliabilityPrimitives},
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " second-half tests passed\n";
    return failures == 0 ? 0 : 1;
}
