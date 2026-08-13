#include "integrations/memory_adapters.h"
#include "integrations/logos_adapters.h"
#include "security/crypto.h"
#include "services/contact_rules.h"
#include "services/messaging_service.h"
#include "services/receipt_service.h"
#include "services/spending_controller.h"
#include "services/storage_service.h"

#include <functional>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using bonded::ApprovalState;
using bonded::ContactRules;
using bonded::Crypto;
using bonded::DomainError;
using bonded::Json;
using bonded::LogosMessagingAdapter;
using bonded::LogosStorageAdapter;
using bonded::MemoryMessagingAdapter;
using bonded::MemoryProgramAdapter;
using bonded::MemoryStorageAdapter;
using bonded::MemoryWalletAdapter;
using bonded::MessagingService;
using bonded::ReceiptService;
using bonded::SettlementOutcome;
using bonded::SettlementReceipt;
using bonded::SignedEnvelope;
using bonded::SpendingController;
using bonded::SpendingPolicy;
using bonded::StorageService;

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

void testMessaging()
{
    MemoryMessagingAdapter adapter;
    MessagingService inbox(adapter, "lez-devnet");
    MessagingService owner(adapter, "lez-devnet");
    const auto [signing_private_key, signing_public_key] = Crypto::generateEd25519KeyPair();
    const auto [owner_encryption_private_key, owner_encryption_public_key] =
        Crypto::generateX25519KeyPair();
    const auto [wrong_encryption_private_key, wrong_encryption_public_key] =
        Crypto::generateX25519KeyPair();
    static_cast<void>(wrong_encryption_public_key);
    const std::string plaintext = "accept:message-1";
    SignedEnvelope envelope{"bonded-inbox/envelope/v2", "lez-devnet", "event-1", "owner",
                            "inbox", "owner/topic", Crypto::randomHex(16), 2000,
                            "", "", "", "", "", ""};
    envelope = MessagingService::sealAndSign(envelope, plaintext,
                                             owner_encryption_public_key,
                                             signing_private_key, signing_public_key);
    check(MessagingService::verify(envelope, 1000, "lez-devnet"),
          "valid messaging envelope failed verification");
    const Json wire_envelope = envelope;
    const auto serialized = wire_envelope.dump();
    check(!wire_envelope.contains("payload"),
          "owner-channel payload field remained in the v2 wire schema");
    check(serialized.find(plaintext) == std::string::npos,
          "owner-channel plaintext appeared in serialized transport bytes");

    std::size_t deliveries = 0;
    std::uint64_t owner_clock = 1000;
    owner.subscribe("owner/topic", [&] { return owner_clock; }, "inbox",
                    owner_encryption_private_key, "owner",
                    signing_public_key,
                    [&](const SignedEnvelope& received, const std::string& opened) {
                        check(received.sender == "owner" && opened == plaintext,
                              "owner-channel identity or plaintext changed");
                        ++deliveries;
                    });
    inbox.send(envelope, 1000);
    inbox.send(envelope, 1000);
    check(deliveries == 1, "duplicate messaging envelope was processed twice");

    auto tampered = envelope;
    tampered.ciphertext[0] = tampered.ciphertext[0] == '0' ? '1' : '0';
    check(!MessagingService::verify(tampered, 1000, "lez-devnet"),
          "tampered messaging envelope verified");
    check(!MessagingService::verify(envelope, 2001, "lez-devnet"),
          "expired messaging envelope verified");
    auto legacy = envelope;
    legacy.protocol = "bonded-inbox/envelope/v1";
    check(!MessagingService::verify(legacy, 1000, "lez-devnet"),
          "legacy plaintext-era envelope protocol verified");
    expectDomainError([&] { inbox.send(envelope, 2001); },
                      "expired messaging envelope was sent");
    owner_clock = 2001;
    SignedEnvelope later_envelope{"bonded-inbox/envelope/v2", "lez-devnet", "event-2", "owner",
                                  "inbox", "owner/topic", Crypto::randomHex(16), 2000,
                                  "", "", "", "", "", ""};
    later_envelope = MessagingService::sealAndSign(
        later_envelope, "deny:message-2", owner_encryption_public_key,
        signing_private_key, signing_public_key);
    expectDomainError([&] { adapter.send("owner/topic", Json(later_envelope).dump()); },
                      "long-lived subscription accepted a newly expired envelope");
    try {
        adapter.send("owner/topic", "{not-json");
        throw std::runtime_error("malformed owner-channel JSON was accepted");
    } catch (const DomainError& error) {
        check(std::string(error.what()) == "received invalid messaging envelope",
              "owner channel exposed parser details");
    }
    expectDomainError(
        [&] {
            MessagingService::open(envelope, 1000, "lez-devnet", "other-recipient",
                                   owner_encryption_private_key, "owner", signing_public_key);
        },
        "wrong owner-channel recipient opened an envelope");
    expectDomainError(
        [&] {
            MessagingService::open(envelope, 1000, "lez-devnet", "inbox",
                                   owner_encryption_private_key, "other-sender",
                                   signing_public_key);
        },
        "wrong owner-channel sender identity opened an envelope");
    expectDomainError(
        [&] {
            MessagingService::open(envelope, 1000, "lez-devnet", "inbox",
                                   wrong_encryption_private_key, "owner", signing_public_key);
        },
        "wrong owner-channel encryption key opened an envelope");
    const auto [other_signing_private_key, other_signing_public_key] =
        Crypto::generateEd25519KeyPair();
    static_cast<void>(other_signing_private_key);
    expectDomainError(
        [&] {
            MessagingService::open(envelope, 1000, "lez-devnet", "inbox",
                                   owner_encryption_private_key, "owner",
                                   other_signing_public_key);
        },
        "unpinned owner signing key opened an envelope");

    adapter.join("group-1");
    check(!adapter.createGroup({"owner", "inbox"}).empty(), "messaging group was not created");
}

void testStorage()
{
    MemoryStorageAdapter adapter;
    StorageService storage(adapter);
    const auto key = Crypto::randomHex(32);
    const auto entry = storage.upload("classified attachment", "brief.txt", key);
    check(entry.address.starts_with("sha256:"), "storage address is not content-derived");
    check(storage.download(entry.address, key) == "classified attachment",
          "storage plaintext did not round trip");
    check(storage.list().size() == 1, "storage list omitted an upload");
    const auto grant = storage.share(entry.address, "processor", 2000, "wrapped-key-fixture");
    check(grant.recipient == "processor", "storage grant recipient changed");
    check(storage.canAccess(entry.address, "processor", 1999), "valid storage grant denied");
    check(!storage.canAccess(entry.address, "processor", 2001), "expired storage grant allowed");
    expectDomainError([&] { storage.download(entry.address, Crypto::randomHex(32)); },
                      "wrong storage key decrypted data");
}

void testLogosMessagingAdapter()
{
    std::vector<std::string> subscriptions;
    std::string sent_topic;
    std::string sent_payload;
    LogosMessagingAdapter adapter(
        [&](const std::string& topic, const std::string& payload) {
            sent_topic = topic;
            sent_payload = payload;
            return "delivery-request-1";
        },
        [&](const std::string& topic) { subscriptions.push_back(topic); });

    check(adapter.send("bonded/private", "sealed") == "delivery-request-1" &&
              sent_topic == "bonded/private" && sent_payload == "sealed",
          "Logos Delivery send callback was not used");
    std::string received;
    adapter.subscribe("bonded/private", [&](const auto& payload) { received = payload; });
    adapter.subscribe("bonded/private", [&](const auto&) {});
    check(subscriptions.size() == 1 && subscriptions.front() == "bonded/private",
          "Logos Delivery topic was subscribed more than once");
    adapter.receive("bonded/private", "sealed-reply");
    check(received == "sealed-reply", "Logos Delivery event was not dispatched");

    const auto first_group = adapter.createGroup({"owner", "inbox", "owner"});
    const auto second_group = adapter.createGroup({"inbox", "owner"});
    check(first_group == second_group && first_group.size() == 32,
          "Logos Delivery group topic was not deterministic");
    check(subscriptions.back() == "bonded/group/" + first_group,
          "Logos Delivery group did not subscribe to its derived topic");

    std::size_t attempts = 0;
    LogosMessagingAdapter retrying(
        [](const std::string&, const std::string&) { return "request"; },
        [&](const std::string&) {
            if (++attempts == 1) {
                throw DomainError("subscription unavailable");
            }
        });
    expectDomainError([&] { retrying.subscribe("retry/topic", [](const auto&) {}); },
                      "failed Logos Delivery subscription did not fail closed");
    retrying.subscribe("retry/topic", [](const auto&) {});
    check(attempts == 2, "failed Logos Delivery subscription was not retried");

    std::mutex subscribe_mutex;
    std::condition_variable subscribe_changed;
    bool release_first = false;
    std::size_t concurrent_attempts = 0;
    LogosMessagingAdapter concurrent(
        [](const std::string&, const std::string&) { return "request"; },
        [&](const std::string&) {
            std::unique_lock lock(subscribe_mutex);
            if (++concurrent_attempts == 1) {
                subscribe_changed.notify_all();
                subscribe_changed.wait(lock, [&] { return release_first; });
                throw DomainError("first concurrent subscription failed");
            }
        });
    bool first_failed = false;
    bool second_succeeded = false;
    std::thread first([&] {
        try {
            concurrent.subscribe("concurrent/topic", [](const auto&) {});
        } catch (const DomainError&) {
            first_failed = true;
        }
    });
    {
        std::unique_lock lock(subscribe_mutex);
        subscribe_changed.wait(lock, [&] { return concurrent_attempts == 1; });
    }
    std::thread second([&] {
        concurrent.subscribe("concurrent/topic", [](const auto&) {});
        second_succeeded = true;
    });
    {
        std::lock_guard lock(subscribe_mutex);
        release_first = true;
    }
    subscribe_changed.notify_all();
    first.join();
    second.join();
    check(first_failed && second_succeeded && concurrent_attempts == 2,
          "concurrent Logos Delivery subscription did not fail and retry safely");

    bool healthy_handler_ran = false;
    concurrent.subscribe("handler/topic", [](const auto&) {
        throw DomainError("handler failed");
    });
    concurrent.subscribe("handler/topic", [&](const auto&) { healthy_handler_ran = true; });
    expectDomainError([&] { concurrent.receive("handler/topic", "sealed"); },
                      "failing Delivery handler was hidden");
    check(healthy_handler_ran, "failing Delivery handler blocked later handlers");
}

void testLogosStorageAdapter()
{
    LogosStorageAdapter* bridge = nullptr;
    std::thread upload_event;
    std::thread download_event;
    LogosStorageAdapter adapter(
        [&](const std::string& payload) {
            check(payload == "ciphertext", "Logos Storage upload payload changed");
            upload_event = std::thread([&] {
                bridge->uploadDone(
                    R"({"success":true,"sessionId":"upload-1","cid":"cid-1"})");
            });
            return "upload-1";
        },
        [&](const std::string& cid) {
            check(cid == "cid-1", "Logos Storage download CID changed");
            download_event = std::thread([&] {
                bridge->downloadProgress(
                    R"({"success":true,"sessionId":"cid-1","chunk":"Y2lwaGVy"})");
                bridge->downloadProgress(
                    R"({"success":true,"sessionId":"cid-1","chunk":"dGV4dA=="})");
                bridge->downloadDone(R"({"success":true,"sessionId":"cid-1"})");
            });
            return cid;
        },
        [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) {}, std::chrono::seconds(1));
    bridge = &adapter;

    check(adapter.put("ciphertext") == "cid-1", "Logos Storage upload did not return CID");
    upload_event.join();
    check(adapter.get("cid-1") == "ciphertext",
          "Logos Storage download chunks did not round trip");
    download_event.join();

    LogosStorageAdapter* immediate_bridge = nullptr;
    LogosStorageAdapter immediate_adapter(
        [&](const std::string&) {
            immediate_bridge->uploadDone(
                R"({"success":true,"sessionId":"immediate","cid":"cid-now"})");
            return "immediate";
        },
        [](const std::string& cid) { return cid; }, [](const std::string&) {},
        [](const std::string&) {}, [](const std::string&) {}, std::chrono::seconds(1));
    immediate_bridge = &immediate_adapter;
    check(immediate_adapter.put("ciphertext") == "cid-now",
          "Logos Storage lost an immediate completion event");

    std::string cancelled;
    LogosStorageAdapter timeout_adapter(
        [](const std::string&) { return "slow-upload"; },
        [](const std::string& cid) { return cid; },
        [&](const std::string& session) { cancelled = session; },
        [](const std::string&) {}, [](const std::string&) {},
        std::chrono::milliseconds(5));
    expectDomainError([&] { timeout_adapter.put("ciphertext"); },
                      "Logos Storage timeout did not fail closed");
    check(cancelled == "slow-upload", "Logos Storage timeout did not cancel its session");

    expectDomainError([&] { adapter.uploadDone("not-json"); },
                      "malformed Logos Storage event was accepted");
    expectDomainError(
        [&] { adapter.uploadDone(R"({"success":"yes","sessionId":"typed"})"); },
        "wrong-typed Logos Storage success was accepted");
    expectDomainError(
        [&] {
            adapter.downloadProgress(
                R"({"success":true,"sessionId":"typed","chunk":42})");
        },
        "wrong-typed Logos Storage chunk was accepted");
    expectDomainError(
        [&] {
            adapter.downloadProgress(
                R"({"success":true,"sessionId":"malformed","chunk":"%%%="})");
        },
        "invalid Logos Storage base64 was accepted");
    expectDomainError(
        [&] {
            adapter.downloadProgress(
                R"({"success":true,"sessionId":"malformed","chunk":"Zh=="})");
        },
        "non-canonical Logos Storage base64 was accepted");

    LogosStorageAdapter* orphan_bridge = nullptr;
    bool first_upload = true;
    LogosStorageAdapter orphan_adapter(
        [&](const std::string&) {
            if (first_upload) {
                first_upload = false;
                orphan_bridge->uploadDone(
                    R"({"success":true,"sessionId":"reused","cid":"stale"})");
                throw DomainError("upload start failed");
            }
            orphan_bridge->uploadDone(
                R"({"success":true,"sessionId":"reused","cid":"fresh"})");
            return "reused";
        },
        [](const std::string& cid) { return cid; }, [](const std::string&) {},
        [](const std::string&) {}, [](const std::string&) {}, std::chrono::seconds(1));
    orphan_bridge = &orphan_adapter;
    expectDomainError([&] { orphan_adapter.put("ciphertext"); },
                      "failed upload start was accepted");
    check(orphan_adapter.put("ciphertext") == "fresh",
          "failed upload start left a reusable completion event");

    std::mutex duplicate_mutex;
    std::condition_variable duplicate_changed;
    std::size_t duplicate_started = 0;
    LogosStorageAdapter duplicate_adapter(
        [&](const std::string&) {
            std::unique_lock lock(duplicate_mutex);
            ++duplicate_started;
            duplicate_changed.notify_all();
            duplicate_changed.wait(lock, [&] { return duplicate_started == 2; });
            return "duplicate";
        },
        [](const std::string& cid) { return cid; }, [](const std::string&) {},
        [](const std::string&) {}, [](const std::string&) {}, std::chrono::seconds(1));
    std::size_t duplicate_failures = 0;
    auto duplicate_put = [&] {
        try {
            static_cast<void>(duplicate_adapter.put("ciphertext"));
        } catch (const DomainError&) {
            std::lock_guard lock(duplicate_mutex);
            ++duplicate_failures;
        }
    };
    std::thread duplicate_first(duplicate_put);
    std::thread duplicate_second(duplicate_put);
    duplicate_first.join();
    duplicate_second.join();
    check(duplicate_failures == 2,
          "duplicate Logos Storage session did not fail both operations");

    LogosStorageAdapter* oversized_bridge = nullptr;
    std::string oversized_cancelled;
    std::thread oversized_event;
    LogosStorageAdapter oversized_adapter(
        [](const std::string&) { return "unused"; },
        [&](const std::string&) {
            oversized_event = std::thread([&] {
                std::string chunk(5592407, 'A');
                chunk += '=';
                oversized_bridge->downloadProgress(
                    Json{{"success", true},
                         {"sessionId", "oversized"},
                         {"chunk", chunk}}
                        .dump());
            });
            return "oversized";
        },
        [](const std::string&) {},
        [&](const std::string& session) { oversized_cancelled = session; },
        [](const std::string&) {}, std::chrono::seconds(2));
    oversized_bridge = &oversized_adapter;
    expectDomainError([&] { oversized_adapter.get("cid-oversized"); },
                      "oversized Logos Storage download was accepted");
    oversized_event.join();
    check(oversized_cancelled == "oversized",
          "oversized Logos Storage download was not cancelled");
}

void testSpending()
{
    MemoryWalletAdapter wallet(1000);
    SpendingController spending(wallet, SpendingPolicy{100, 250, 1000, 100});
    const auto automatic = spending.propose("translation-agent", 75, 1000);
    check(automatic.state == ApprovalState::Executed && wallet.balance() == 925,
          "below-limit spend was not autonomous");

    const auto pending = spending.propose("expensive-agent", 150, 1010);
    check(pending.state == ApprovalState::Pending && wallet.balance() == 925,
          "above-limit spend executed before approval");
    const auto approved = spending.approve(pending.id, 1020);
    check(approved.state == ApprovalState::Executed && wallet.balance() == 775,
          "approved spend did not execute once");
    check(spending.approve(pending.id, 1030).transfer_id == approved.transfer_id,
          "duplicate approval created another transfer");

    const auto expiring = spending.propose("offline-owner", 200, 1040);
    spending.expire(1141);
    check(spending.get(expiring.id)->state == ApprovalState::Expired,
          "unanswered approval did not expire");
    expectDomainError([&] { spending.approve(expiring.id, 1141); },
                      "expired approval executed");
}

void testReceipt()
{
    const auto [private_key, public_key] = Crypto::generateEd25519KeyPair();
    SettlementReceipt receipt{"bonded-inbox/receipt/v1",
                              "lez-devnet",
                              "message-1",
                              "bond:message-1",
                              "policy-hash",
                              "transaction-1",
                              Crypto::sha256("sender-address"),
                              25,
                              1200,
                              SettlementOutcome::RefundAccepted,
                              "",
                              ""};
    receipt = ReceiptService::sign(receipt, private_key, public_key);
    check(ReceiptService::verify(receipt), "valid private receipt failed verification");
    receipt.amount = 26;
    check(!ReceiptService::verify(receipt), "tampered receipt verified");
}

void testContactRules()
{
    ContactRules rules(2, 60);
    rules.trust("authenticated-contact");
    check(rules.isTrusted("authenticated-contact"), "trusted contact missing");
    rules.revoke("authenticated-contact");
    check(!rules.isTrusted("authenticated-contact"), "revoked contact remained trusted");
    check(rules.allow("sender-commitment", 1000), "first message was rate-limited");
    check(rules.allow("sender-commitment", 1001), "second message was rate-limited");
    check(!rules.allow("sender-commitment", 1002), "spam burst exceeded rate limit");
    check(rules.allow("sender-commitment", 1061), "rate window did not recover");
}

void testProgramAdapter()
{
    MemoryProgramAdapter programs;
    const auto program_id = programs.deploy("programs/bonded-inbox/Cargo.toml");
    const auto transaction = programs.call(program_id, "lock", Json{{"amount", 25}});
    check(!transaction.empty(), "program call returned no transaction id");
    check(programs.query(program_id, Json::object()).at("state").at("lock").at("amount") == 25,
          "program query did not return state");
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"messaging", testMessaging}, {"storage", testStorage},
        {"logos messaging adapter", testLogosMessagingAdapter},
        {"logos storage adapter", testLogosStorageAdapter},
        {"spending", testSpending},   {"receipt", testReceipt},
        {"contact rules", testContactRules}, {"program adapter", testProgramAdapter},
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
    std::cout << (tests.size() - failures) << '/' << tests.size() << " service tests passed\n";
    return failures == 0 ? 0 : 1;
}
