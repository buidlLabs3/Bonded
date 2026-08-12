#include "domain/policy.h"
#include "domain/state_machine.h"
#include "runtime/skill_registry.h"
#include "security/crypto.h"
#include "services/bond_service.h"
#include "services/inbox_service.h"
#include "storage/database.h"

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <functional>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using bonded::AttachmentPolicy;
using bonded::BondRecord;
using bonded::BondService;
using bonded::Crypto;
using bonded::Database;
using bonded::DomainError;
using bonded::InboxPolicy;
using bonded::InboxService;
using bonded::Json;
using bonded::MessageRecord;
using bonded::MessageState;
using bonded::MessageStateMachine;
using bonded::PolicyService;
using bonded::Profile;
using bonded::SettlementOutcome;
using bonded::SkillDefinition;
using bonded::SkillRegistry;
using bonded::Submission;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function> void expectDomainError(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const DomainError&) {
        return;
    }
    throw std::runtime_error(message);
}

InboxPolicy signedPolicy(std::uint64_t version = 1)
{
    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    InboxPolicy policy{"inbox-alpha",
                       "owner-address",
                       "community-sink",
                       "bond-free:help",
                       "lez-devnet",
                       version,
                       25,
                       3600,
                       1000,
                       AttachmentPolicy{{"text/plain", "application/pdf"}, 1024 * 1024, 1},
                       "",
                       ""};
    return PolicyService::sign(std::move(policy), private_key, public_key);
}

void testStateMachine()
{
    MessageRecord message{"m1", "sender", "hash", 1, 25, 100, MessageState::Created, 0, std::nullopt};
    MessageStateMachine::transition(message, MessageState::BondPending, 0);
    MessageStateMachine::transition(message, MessageState::Bonded, 1);
    MessageStateMachine::transition(message, MessageState::DeliveryPending, 2);
    MessageStateMachine::transition(message, MessageState::PendingReview, 3);
    MessageStateMachine::transition(message, MessageState::Accepted, 4);
    message.settlement = SettlementOutcome::RefundAccepted;
    MessageStateMachine::transition(message, MessageState::Settled, 5);
    check(message.revision == 6, "state transition revision did not advance");
    expectDomainError(
        [&] { MessageStateMachine::transition(message, MessageState::Rejected, message.revision); },
        "terminal state accepted an illegal transition");

    MessageRecord unsettled{
        "m2", "sender", "hash", 1, 25, 100, MessageState::Rejected, 7, std::nullopt};
    expectDomainError(
        [&] { MessageStateMachine::transition(unsettled, MessageState::Settled, 7); },
        "settled without a financial outcome");
}

void testCrypto()
{
    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    const auto signature = Crypto::signEd25519(private_key, "bonded");
    check(Crypto::verifyEd25519(public_key, "bonded", signature), "valid signature failed");
    check(!Crypto::verifyEd25519(public_key, "tampered", signature),
          "tampered signature verified");

    const auto key = Crypto::randomHex(32);
    const auto encrypted = Crypto::encryptAes256Gcm(key, "private message", "message:m1");
    check(Crypto::decryptAes256Gcm(key, encrypted, "message:m1") == "private message",
          "authenticated encryption round trip failed");
    expectDomainError([&] { Crypto::decryptAes256Gcm(key, encrypted, "message:m2"); },
                      "wrong associated data decrypted");

    const auto [alice_private, alice_public] = Crypto::generateX25519KeyPair();
    const auto [bob_private, bob_public] = Crypto::generateX25519KeyPair();
    check(Crypto::deriveX25519(alice_private, bob_public) ==
              Crypto::deriveX25519(bob_private, alice_public),
          "X25519 peers did not derive the same shared secret");
    check(Crypto::hkdfSha256(Crypto::deriveX25519(alice_private, bob_public), "context-a") ==
              Crypto::hkdfSha256(Crypto::deriveX25519(bob_private, alice_public), "context-a"),
          "HKDF peers did not derive the same context-bound key");
    check(Crypto::hkdfSha256(Crypto::deriveX25519(alice_private, bob_public), "context-a") !=
              Crypto::hkdfSha256(Crypto::deriveX25519(alice_private, bob_public), "context-b"),
          "HKDF did not bind the encryption context");
    expectDomainError([&] { Crypto::deriveX25519(alice_private, "00"); },
                      "invalid X25519 public key was accepted");
}

void testPolicy()
{
    auto policy = signedPolicy();
    check(PolicyService::verify(policy), "signed policy did not verify");
    policy.sink_address = policy.owner_address;
    check(!PolicyService::verify(policy), "owner-profit policy verified");

    auto current = signedPolicy(1);
    auto successor = signedPolicy(2);
    PolicyService::requireSuccessor(current, successor);
    successor.version = 4;
    expectDomainError([&] { PolicyService::requireSuccessor(current, successor); },
                      "policy skipped a version");
}

void testDatabase()
{
    Database database(":memory:");
    database.migrate();
    auto policy = signedPolicy();
    database.savePolicy(policy);
    check(database.latestPolicy(policy.inbox_id)->version == 1, "policy was not persisted");

    MessageRecord message{"m1", "sender", PolicyService::hash(policy), 1, 25, 2000,
                          MessageState::Created, 0, std::nullopt};
    check(database.createMessage(message, "idem-1"), "message was not created");
    check(!database.createMessage(message, "idem-1"), "duplicate message was created");
    MessageStateMachine::transition(message, MessageState::BondPending, 0);
    database.updateMessage(message, 0);
    check(database.message("m1")->revision == 1, "message revision was not persisted");
    expectDomainError([&] { database.updateMessage(message, 0); },
                      "stale database revision succeeded");

    database.enqueue("owner", "redacted-event");
    const auto outbox = database.pendingOutbox(10);
    check(outbox.size() == 1, "outbox record missing");
    database.acknowledgeOutbox(outbox.front().id);
    check(database.pendingOutbox(10).empty(), "outbox acknowledgement failed");
    database.recordProcessedEvent("event-1");
    check(database.hasProcessedEvent("event-1"), "processed-event record missing");

    BondRecord bond{
        "bond:m1", "m1", "sender", "owner", "sink", "policy", 50, 2000, std::nullopt};
    check(database.createBond(bond), "bond was not created");
    check(!database.createBond(bond), "duplicate bond was created");
    check(database.bond("bond:m1").has_value() &&
              Json(*database.bond("bond:m1")) == Json(bond),
          "bond was not persisted");
    bond.outcome = SettlementOutcome::RefundAccepted;
    check(database.settleBond(bond), "bond settlement was not persisted");
    check(!database.settleBond(bond), "bond settled twice");
    check(database.bondCount() == 1, "persisted bond count is wrong");
    check(database.enqueueOnce("receipt:m1", "receipt", "payload"),
          "deduplicated outbox record was not created");
    check(!database.enqueueOnce("receipt:m1", "receipt", "payload"),
          "deduplicated outbox record was created twice");
}

void testProcessInterruptionRecovery()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("bonded-inbox-recovery-" + std::to_string(::getpid()));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto database_path = root / "recovery.db";

    int readiness[2]{};
    check(::pipe(readiness) == 0, "could not create process readiness pipe");
    const auto writer = ::fork();
    if (writer < 0) {
        ::close(readiness[0]);
        ::close(readiness[1]);
        throw std::runtime_error("could not fork recovery writer");
    }
    if (writer == 0) {
        ::close(readiness[0]);
        try {
            Database database(database_path);
            database.migrate();
            database.enqueue("bonded/recovery/first", "public-event-1");
            database.enqueue("bonded/recovery/second", "public-event-2");
            database.recordProcessedEvent("incoming-event-1");
            const char ready = '1';
            if (::write(readiness[1], &ready, 1) != 1) {
                ::_exit(3);
            }
            for (;;) {
                ::pause();
            }
        } catch (...) {
            ::_exit(2);
        }
    }

    ::close(readiness[1]);
    char ready = 0;
    pollfd readiness_poll{readiness[0], POLLIN, 0};
    const auto poll_result = ::poll(&readiness_poll, 1, 10000);
    const auto ready_bytes = poll_result > 0 ? ::read(readiness[0], &ready, 1) : -1;
    ::close(readiness[0]);
    if (ready_bytes != 1 || ready != '1') {
        ::kill(writer, SIGKILL);
        ::waitpid(writer, nullptr, 0);
        throw std::runtime_error("recovery writer exited before durable readiness");
    }
    check(::kill(writer, SIGKILL) == 0, "could not interrupt recovery writer");
    int writer_status = 0;
    check(::waitpid(writer, &writer_status, 0) == writer,
          "could not reap interrupted recovery writer");
    check(WIFSIGNALED(writer_status) && WTERMSIG(writer_status) == SIGKILL,
          "recovery writer did not terminate from SIGKILL");

    std::int64_t acknowledged_id = 0;
    {
        Database database(database_path);
        database.migrate();
        const auto pending = database.pendingOutbox(10);
        check(pending.size() == 2, "restart did not recover both pending outbox records");
        check(pending[0].topic == "bonded/recovery/first" &&
                  pending[0].payload == "public-event-1" &&
                  pending[1].topic == "bonded/recovery/second" &&
                  pending[1].payload == "public-event-2",
              "restart changed durable outbox ordering or content");
        check(database.hasProcessedEvent("incoming-event-1"),
              "restart lost the processed-event replay marker");
        database.recordProcessedEvent("incoming-event-1");
        check(database.hasProcessedEvent("incoming-event-1"),
              "duplicate replay marker was not idempotent");
        acknowledged_id = pending.front().id;
        database.acknowledgeOutbox(acknowledged_id);
    }

    {
        Database database(database_path);
        database.migrate();
        const auto pending = database.pendingOutbox(10);
        check(pending.size() == 1 && pending.front().topic == "bonded/recovery/second",
              "outbox acknowledgement was not durable across a second restart");
        check(pending.front().id != acknowledged_id,
              "acknowledged outbox record was replayed after restart");
        check(database.hasProcessedEvent("incoming-event-1"),
              "processed-event marker disappeared after a second restart");
    }
}

void testBondService()
{
    Database database(":memory:");
    database.migrate();
    BondService bonds(database);
    BondRecord bond{
        "bond:m1", "m1", "sender", "owner", "sink", "policy", 50, 2000, std::nullopt};
    bonds.lock(bond);
    const auto refund = bonds.settle(bond.id, SettlementOutcome::RefundAccepted);
    check(refund.destination == "sender" && refund.amount == 50, "refund destination is wrong");
    check(bonds.settle(bond.id, SettlementOutcome::RefundAccepted).duplicate,
          "duplicate settlement was not idempotent");
    expectDomainError([&] { bonds.settle(bond.id, SettlementOutcome::SinkRejected); },
                      "conflicting settlement succeeded");

    bond.id = "bond:m2";
    bond.message_id = "m2";
    bond.outcome.reset();
    bonds.lock(bond);
    check(bonds.settle(bond.id, SettlementOutcome::SinkRejected).destination == "sink",
          "rejection did not use fixed sink");
    bond.id = "bond:m3";
    bond.owner = bond.sink;
    expectDomainError([&] { bonds.lock(bond); }, "owner-profit bond was accepted");
}

void testSkillRegistry()
{
    SkillRegistry registry;
    registry.registerSkill(SkillDefinition{"echo",
                                           "echo input",
                                           Json{{"type", "object"}},
                                           Json{{"type", "object"}},
                                           {Profile::Inbox},
                                           [](const Json& input) { return input; }});
    check(registry.has("echo", Profile::Inbox), "allowed skill not found");
    check(!registry.has("echo", Profile::Settlement), "skill escaped profile allowlist");
    check(registry.invoke("echo", Profile::Inbox, Json{{"value", 3}}).at("value") == 3,
          "skill dispatch failed");
    expectDomainError([&] { registry.invoke("echo", Profile::Settlement, Json::object()); },
                      "disallowed skill executed");
}

void testInboxLifecycle()
{
    Database database(":memory:");
    database.migrate();
    BondService bonds(database);
    InboxService inbox(database, bonds);
    const auto policy = signedPolicy();
    inbox.publishPolicy(policy);
    const auto policy_hash = PolicyService::hash(policy);

    Submission legitimate{"inbox-alpha", "message-1", "idem-1", "sender-1", policy_hash,
                          "bond:message-1", 25, 1100, false, 1, 512, "text/plain"};
    const auto pending = inbox.submit(legitimate);
    check(pending.state == MessageState::PendingReview, "message did not reach review");
    check(inbox.submit(legitimate).revision == pending.revision, "duplicate changed message state");
    const auto accepted = inbox.decide("message-1", MessageState::Accepted, false, false);
    check(accepted.state == MessageState::Settled &&
              accepted.settlement == SettlementOutcome::RefundAccepted,
          "accepted message was not refunded and settled");
    check(inbox.decide("message-1", MessageState::Accepted, false, false).revision ==
              accepted.revision,
          "duplicate owner decision changed terminal message state");
    expectDomainError(
        [&] { inbox.decide("message-1", MessageState::Rejected, true, false); },
        "conflicting owner decision changed terminal message state");

    Submission spam{"inbox-alpha", "message-2", "idem-2", "sender-2", policy_hash,
                    "bond:message-2", 25, 1100, false, 0, 0, ""};
    inbox.submit(spam);
    expectDomainError(
        [&] { inbox.decide("message-2", MessageState::Rejected, false, false); },
        "classifier-only rejection succeeded");
    const auto rejected = inbox.decide("message-2", MessageState::Rejected, true, false);
    check(rejected.settlement == SettlementOutcome::SinkRejected,
          "explicit spam rejection did not settle to sink");

    Submission trusted{"inbox-alpha", "message-3", "idem-3", "known-contact", policy_hash,
                       "", 0, 1100, true, 0, 0, ""};
    const auto trusted_pending = inbox.submit(trusted);
    check(trusted_pending.bond_amount == 0, "trusted contact did not bypass bond");
    check(inbox.decide("message-3", MessageState::Accepted, false, false).state ==
              MessageState::Settled,
          "trusted contact did not settle");

    Submission mismatched_bond{"inbox-alpha", "message-4", "idem-4", "sender-4", policy_hash,
                               "bond:different-message", 25, 1100, false, 0, 0, ""};
    expectDomainError([&] { inbox.submit(mismatched_bond); },
                      "mismatched bond identifier was accepted");

    Submission stale = legitimate;
    stale.message_id = "message-5";
    stale.idempotency_key = "idem-5";
    stale.bond_id = "bond:message-5";
    stale.policy_hash = "bad-hash";
    expectDomainError([&] { inbox.submit(stale); }, "stale policy commitment was accepted");
}

void testInboxRestartRecovery()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("bonded-inbox-lifecycle-recovery-" + std::to_string(::getpid()));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};
    std::filesystem::remove_all(root);
    std::filesystem::create_directory(root);
    const auto database_path = root / "lifecycle.db";
    const auto policy = signedPolicy();
    const auto policy_hash = PolicyService::hash(policy);
    const Submission submission{"inbox-alpha", "restart-message", "restart-idem", "sender",
                                policy_hash, "bond:restart-message", 25, 1100, false, 0, 0, ""};

    {
        Database database(database_path);
        database.migrate();
        BondService bonds(database);
        InboxService inbox(database, bonds);
        inbox.publishPolicy(policy);
        check(inbox.submit(submission).state == MessageState::PendingReview,
              "restart fixture did not reach pending review");
        check(bonds.size() == 1, "restart fixture did not persist its bond");
    }

    {
        Database database(database_path);
        database.migrate();
        BondService bonds(database);
        InboxService inbox(database, bonds);
        check(bonds.get("bond:restart-message").has_value(),
              "restart lost the pending message bond");
        const auto settled =
            inbox.decide("restart-message", MessageState::Accepted, false, false);
        check(settled.state == MessageState::Settled &&
                  settled.settlement == SettlementOutcome::RefundAccepted,
              "restarted inbox could not settle its pending message");
    }

    {
        Database database(database_path);
        database.migrate();
        BondService bonds(database);
        InboxService inbox(database, bonds);
        const auto settled =
            inbox.decide("restart-message", MessageState::Accepted, false, false);
        check(settled.state == MessageState::Settled &&
                  bonds.get("bond:restart-message")->outcome ==
                      SettlementOutcome::RefundAccepted,
              "second restart did not preserve terminal settlement");
        const auto outbox = database.pendingOutbox(20);
        check(std::count_if(outbox.begin(), outbox.end(), [](const auto& record) {
                  return record.topic == "bonded/receipt/sender";
              }) == 1,
              "restart duplicated the settlement receipt");
    }
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"state machine", testStateMachine}, {"cryptography", testCrypto},
        {"policy", testPolicy},             {"database", testDatabase},
        {"process recovery", testProcessInterruptionRecovery},
        {"bond service", testBondService},  {"skill registry", testSkillRegistry},
        {"inbox lifecycle", testInboxLifecycle},
        {"inbox restart recovery", testInboxRestartRecovery},
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
    std::cout << (tests.size() - failures) << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
