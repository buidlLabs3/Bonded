#include "integrations/memory_adapters.h"
#include "runtime/default_skill_catalog.h"
#include "runtime/reliability.h"
#include "runtime/skill_registry.h"
#include "security/crypto.h"
#include "services/a2a_service.h"
#include "services/configuration_service.h"
#include "services/triage_service.h"

#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
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
using bonded::MemoryMessagingAdapter;
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
    std::optional<Json> persisted;
    ConfigurationService configuration(Json{{"rate_limit", 5}}, public_key,
                                       [&] { return persisted; },
                                       [&](const Json& state) { persisted = state; });
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
    ConfigurationService restarted(Json{{"rate_limit", 1}}, public_key,
                                   [&] { return persisted; });
    check(restarted.snapshot().at("revision") == 1 &&
              restarted.snapshot().at("values").at("rate_limit") == 7,
          "owner configuration did not survive restart");
    check(restarted.updateAuthenticated(Json{{"owner_notifications", false}}, 1)
                      .at("revision") == 2 &&
              restarted.snapshot().at("values").at("owner_notifications") == false,
          "authenticated owner channel could not update durable configuration");
}

AgentCard signedCard(const std::string& id, const std::string& skill,
                     std::uint64_t price, const std::string& network,
                     const std::string& private_key, const std::string& public_key,
                     const std::string& encryption_public_key, std::uint64_t expires_at)
{
    AgentCard card{"a2a/1.0", network, id, public_key, {skill},
                   Json{{"messaging_encryption", "x25519-aes-256-gcm"},
                        {"messaging_encryption_public_key", encryption_public_key},
                        {"payment_recipient", std::string(64, 'b')},
                        {"payment_private_keys",
                         Json{{"nullifier_public_key", std::string(64, 'c')},
                              {"viewing_public_key", std::string(64, 'd')}}}},
                   "/bonded-inbox/1/a2a-task/json", price, expires_at, ""};
    return A2AService::signCard(std::move(card), private_key);
}

void testA2ALifecycle()
{
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    MemoryMessagingAdapter messaging;
    bool settled = false;
    A2AService requester(
        messaging, "logos-local", {}, {}, {}, {},
        [&](const std::string& recipient, const Json& recipient_private_keys,
            std::uint64_t amount,
            std::uint64_t, const std::string& request_id) {
            check(recipient == std::string(64, 'b') && amount == 12,
                  "A2A completion requested the wrong settlement");
            check(recipient_private_keys.at("nullifier_public_key") ==
                      std::string(64, 'c'),
                  "A2A completion omitted private recipient keys");
            if (request_id == "a2a:task-3") {
                return Json{{"id", "spend:a2a:task-3"},
                            {"state", "pending"},
                            {"transfer_id", ""}};
            }
            check(request_id == "a2a:task-1",
                  "A2A completion used the wrong request id");
            settled = true;
            return Json{{"id", "spend:a2a:task-1"},
                        {"state", "executed"},
                        {"transfer_id", std::string(64, 'c')}};
        });
    A2AService provider(messaging, "logos-local");
    const auto [provider_private, provider_public] = Crypto::generateEd25519KeyPair();
    const auto [provider_encryption_private, provider_encryption_public] =
        Crypto::generateX25519KeyPair();
    const auto [peer_private, peer_public] = Crypto::generateEd25519KeyPair();
    const auto [peer_encryption_private, peer_encryption_public] =
        Crypto::generateX25519KeyPair();
    requester.configureTransport("peer", peer_private, peer_public, peer_encryption_private);
    provider.configureTransport("provider", provider_private, provider_public,
                                provider_encryption_private);
    const auto peer_card = signedCard("peer", "private.process", 12, "logos-local",
                                      peer_private, peer_public, peer_encryption_public,
                                      now + 1000);
    const auto provider_card = signedCard("provider", "private.process", 12, "logos-local",
                                          provider_private, provider_public,
                                          provider_encryption_public, now + 1000);
    const Json card_document = provider_card;
    check(card_document.at("supportedInterfaces").at(0).at("protocolVersion") == "1.0" &&
              card_document.at("skills").at(0).at("id") == "private.process" &&
              card_document.at("signatures").at(0).contains("protected") &&
              provider.verifyCard(card_document.get<AgentCard>(), now),
          "A2A 1.0 Agent Card JSON or JWS round trip is invalid");
    auto tampered_card = provider_card;
    ++tampered_card.task_price;
    check(!provider.verifyCard(tampered_card, now),
          "tampered A2A Agent Card passed JWS verification");
    requester.publishCard(peer_card, now);
    provider.publishCard(provider_card, now);
    check(requester.discover("private.process", now).size() == 2 &&
              provider.discover("private.process", now).size() == 2,
          "two A2A agents did not discover one another");

    A2ATask task{"task-1", "peer", "provider", "private.process", Json{{"object", "cid"}},
                 Json::object(), 12, now + 900, A2ATaskState::Working, "", 0};
    const auto working = requester.createTask(task, now);
    check(working.payment_reference.empty() &&
              provider.subscribe(task.id).state == A2ATaskState::Working,
          "A2A task did not arrive without claiming fake escrow");
    provider.requireInput(task.id, now + 1);
    check(requester.subscribe(task.id).state == A2ATaskState::InputRequired,
          "encrypted input-required update did not reach requester");
    check(provider.complete(task.id, Json{{"result", "commitment"}}, now + 2).state ==
              A2ATaskState::Completed,
          "A2A task did not complete");
    check(requester.subscribe(task.id).state == A2ATaskState::Completed,
          "encrypted completion did not reach requester");
    check(settled && requester.subscribe(task.id).payment_reference == std::string(64, 'c') &&
              provider.subscribe(task.id).payment_reference == std::string(64, 'c'),
          "paid A2A completion did not propagate its transfer reference");
    check(provider.complete(task.id, Json{{"ignored", true}}, now + 3).revision == 3,
          "duplicate A2A completion was not idempotent");

    task.id = "task-2";
    requester.createTask(task, now + 4);
    check(requester.cancel(task.id, now + 5).state == A2ATaskState::Canceled &&
              provider.subscribe(task.id).state == A2ATaskState::Canceled,
          "encrypted A2A cancellation did not reach provider");
    task.id = "task-3";
    requester.createTask(task, now + 6);
    provider.complete(task.id, Json{{"result", "approval-required"}}, now + 7);
    check(requester.subscribe(task.id).state == A2ATaskState::InputRequired &&
              requester.subscribe(task.id).payment_reference == "spend:a2a:task-3",
          "above-limit A2A payment did not wait for owner approval");
    requester.recordSettlement("spend:a2a:task-3", std::string(64, 'd'), now + 8);
    check(requester.subscribe(task.id).state == A2ATaskState::Completed &&
              provider.subscribe(task.id).payment_reference == std::string(64, 'd'),
          "owner-approved A2A settlement did not complete on both agents");
    expectFailure([&] { requester.createTask(
                           A2ATask{"bad", "peer", "provider", "private.process",
                                   Json::object(), Json::object(), 99, now + 900,
                                   A2ATaskState::Working, "", 0}, now); },
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
